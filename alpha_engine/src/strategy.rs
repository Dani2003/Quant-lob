//! Production High-Frequency Trading (HFT) Market Making & Alpha Strategy.
//! Incorporates SIMD Order Book Imbalance, Stoikov Micro-Price drift, and
//! Avellaneda-Stoikov inventory risk penalty.

use crate::simd_obi::QuantAlphaSignal;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum StrategyAction {
    Hold,
    QuoteBid { price: u32, qty: u32 },
    QuoteAsk { price: u32, qty: u32 },
    AggressiveBuy { price: u32, qty: u32 },
    AggressiveSell { price: u32, qty: u32 },
    CancelStaleQuotes,
}

#[derive(Debug, Clone)]
pub struct StrategyConfig {
    /// OBI threshold to trigger directional conviction (-1.0 to +1.0)
    pub obi_threshold: f32,
    /// Minimum spread in ticks required to provide liquidity
    pub min_spread_ticks: u32,
    /// Maximum allowed absolute inventory before hard quote shading
    pub max_inventory: i32,
    /// Avellaneda-Stoikov risk aversion parameter (gamma)
    pub inventory_gamma: f64,
    /// Base order quote lot size
    pub quote_lot_size: u32,
}

impl Default for StrategyConfig {
    fn default() -> Self {
        Self {
            obi_threshold: 0.25,
            min_spread_ticks: 2,
            max_inventory: 1000,
            inventory_gamma: 0.005,
            quote_lot_size: 100,
        }
    }
}

/// Real-time Quantitative Market Maker & Micro-Alpha Engine
pub struct HftAlphaStrategy {
    config: StrategyConfig,
    current_inventory: i32,
}

impl HftAlphaStrategy {
    pub fn new(config: StrategyConfig) -> Self {
        Self {
            config,
            current_inventory: 0,
        }
    }

    #[inline(always)]
    pub fn update_inventory(&mut self, delta: i32) {
        self.current_inventory += delta;
    }

    #[inline(always)]
    pub fn inventory(&self) -> i32 {
        self.current_inventory
    }

    /// Evaluates real-time market signals to produce optimal quotes or aggressive takes.
    /// Uses Avellaneda-Stoikov reservation price formula:
    ///   R(s, q, t) = MidPrice - q * gamma * sigma^2
    #[inline(always)]
    pub fn evaluate(&self, signal: &QuantAlphaSignal) -> StrategyAction {
        // If spread is crossed or locked, hold
        if signal.spread_ticks == 0 {
            return StrategyAction::Hold;
        }

        // 1. Avellaneda-Stoikov Inventory Reservation Price Skew
        let inventory_skew = -(self.current_inventory as f64) * self.config.inventory_gamma;
        let reservation_price = signal.micro_price + inventory_skew;

        let best_bid = (signal.mid_price - (signal.spread_ticks as f64 / 2.0)).round() as u32;
        let best_ask = (signal.mid_price + (signal.spread_ticks as f64 / 2.0)).round() as u32;

        let top_obi = signal.obi.levels[0];

        // 2. High Conviction Alpha: Strong Imbalance & Micro-Price Drift
        if top_obi > self.config.obi_threshold && reservation_price > signal.mid_price {
            // Strong upward book momentum: if inventory allows, take liquidity or join best bid
            if self.current_inventory < self.config.max_inventory {
                if top_obi > 0.60 {
                    // Toxic order flow coming: Aggressively take ask before queue depletes
                    return StrategyAction::AggressiveBuy {
                        price: best_ask,
                        qty: self.config.quote_lot_size,
                    };
                } else {
                    // Provide passive liquidity at the inside bid
                    return StrategyAction::QuoteBid {
                        price: best_bid,
                        qty: self.config.quote_lot_size,
                    };
                }
            }
        } else if top_obi < -self.config.obi_threshold && reservation_price < signal.mid_price {
            // Strong downward book momentum
            if self.current_inventory > -self.config.max_inventory {
                if top_obi < -0.60 {
                    // Aggressively cross spread to liquidate / sell
                    return StrategyAction::AggressiveSell {
                        price: best_bid,
                        qty: self.config.quote_lot_size,
                    };
                } else {
                    // Provide passive liquidity at the inside ask
                    return StrategyAction::QuoteAsk {
                        price: best_ask,
                        qty: self.config.quote_lot_size,
                    };
                }
            }
        }

        // 3. Symmetric Two-Sided Market Making when book is balanced
        if signal.spread_ticks >= self.config.min_spread_ticks {
            if self.current_inventory > (self.config.max_inventory / 2) {
                // Excess long: Quote only ask to reduce inventory
                StrategyAction::QuoteAsk {
                    price: best_ask,
                    qty: self.config.quote_lot_size,
                }
            } else if self.current_inventory < -(self.config.max_inventory / 2) {
                // Excess short: Quote only bid to cover inventory
                StrategyAction::QuoteBid {
                    price: best_bid,
                    qty: self.config.quote_lot_size,
                }
            } else {
                // Balanced: Join passive bid
                StrategyAction::QuoteBid {
                    price: best_bid,
                    qty: self.config.quote_lot_size,
                }
            }
        } else {
            StrategyAction::Hold
        }
    }
}

