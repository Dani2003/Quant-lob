"""
Event-Driven LOB Backtesting & Quantitative Simulation Framework.
Simulates high-frequency market making strategies, inventory risk, and real-time TCA.
"""

from dataclasses import dataclass
from typing import List, Dict, Optional
import math
from .engine import PyOrderBook, Side, Top5Depth

@dataclass
class BacktestResult:
    total_ticks: int
    total_trades: int
    total_volume: int
    final_pnl_dollars: float
    max_drawdown_dollars: float
    sharpe_ratio: float
    win_rate: float
    avg_effective_spread_ticks: float
    avg_realized_spread_ticks: float
    adverse_selection_ratio: float
    final_inventory: int

class MarketMakingStrategy:
    """Quantitative High-Frequency Market Maker with Avellaneda-Stoikov Inventory Skew."""

    def __init__(
        self,
        obi_threshold: float = 0.25,
        min_spread_ticks: int = 2,
        max_inventory: int = 1000,
        inventory_gamma: float = 0.005,
        quote_lot_size: int = 100,
        aggressive_obi_threshold: float = 0.60
    ):
        self.obi_threshold = obi_threshold
        self.min_spread_ticks = min_spread_ticks
        self.max_inventory = max_inventory
        self.inventory_gamma = inventory_gamma
        self.quote_lot_size = quote_lot_size
        self.aggressive_obi_threshold = aggressive_obi_threshold
        self.inventory = 0

    def on_tick(self, book: PyOrderBook) -> Optional[Dict]:
        depth = book.get_top5_depth()
        if depth.bid_depth == 0 or depth.ask_depth == 0:
            return None

        best_bid = depth.bid_prices[0]
        best_ask = depth.ask_prices[0]
        bid_vol = depth.bid_volumes[0]
        ask_vol = depth.ask_volumes[0]

        if best_ask <= best_bid:
            return None

        mid_price = (best_bid + best_ask) / 2.0
        spread = best_ask - best_bid
        micro_price = book.calculate_micro_price(best_bid, bid_vol, best_ask, ask_vol)
        _, weighted_obi = book.calculate_obi(depth.bid_volumes, depth.ask_volumes)

        # Avellaneda-Stoikov inventory reservation price skew
        reservation_price = micro_price - (self.inventory * self.inventory_gamma)

        # High-conviction alpha trigger
        if weighted_obi > self.obi_threshold and reservation_price > mid_price:
            if self.inventory < self.max_inventory:
                qty = min(self.quote_lot_size, self.max_inventory - self.inventory)
                action_type = "TAKE_BUY" if weighted_obi > self.aggressive_obi_threshold else "QUOTE_BID"
                return {"action": action_type, "side": Side.BUY, "price": best_ask if action_type == "TAKE_BUY" else best_bid, "qty": qty}
        elif weighted_obi < -self.obi_threshold and reservation_price < mid_price:
            if self.inventory > -self.max_inventory:
                qty = min(self.quote_lot_size, self.max_inventory + self.inventory)
                action_type = "TAKE_SELL" if weighted_obi < -self.aggressive_obi_threshold else "QUOTE_ASK"
                return {"action": action_type, "side": Side.SELL, "price": best_bid if action_type == "TAKE_SELL" else best_ask, "qty": qty}

        # Symmetric quoting: Quote both sides of the spread
        if spread >= self.min_spread_ticks:
            if self.inventory > (self.max_inventory // 2):
                return {"action": "QUOTE_ASK", "side": Side.SELL, "price": best_ask, "qty": self.quote_lot_size}
            elif self.inventory < -(self.max_inventory // 2):
                return {"action": "QUOTE_BID", "side": Side.BUY, "price": best_bid, "qty": self.quote_lot_size}
            else:
                return {
                    "action": "QUOTE_TWO_SIDED",
                    "bid_price": best_bid,
                    "ask_price": best_ask,
                    "qty": self.quote_lot_size
                }

        return None



class LobBacktester:
    """Event-driven simulator running historical or synthetic market data feeds."""

    def __init__(self, strategy: MarketMakingStrategy, markout_horizon_events: int = 50):
        self.strategy = strategy
        self.markout_horizon_events = max(0, markout_horizon_events)
        self.book = PyOrderBook()
        self.cash = 0.0
        self.inventory = 0
        self.pnl_history = []
        self.trades = []
        self.effective_spreads = []
        self.realized_spreads = []
        self.toxic_trades = 0
        self.total_volume = 0

    def run_simulation(self, tick_stream: List[Dict]) -> BacktestResult:
        """
        Processes tick stream:
        Each tick is a dict: {'id': int, 'side': Side, 'price': int, 'qty': int, 'action': 'ADD'|'CANCEL'}
        """
        self.cash = 0.0
        self.inventory = 0
        self.strategy.inventory = 0
        self.pnl_history.clear()
        self.trades.clear()
        self.effective_spreads.clear()
        self.realized_spreads.clear()
        self.toxic_trades = 0
        self.total_volume = 0
        peak_pnl = 0.0
        max_drawdown = 0.0
        active_bid = None
        active_ask = None
        mids_by_event = []

        for idx, tick in enumerate(tick_stream):
            action = tick.get("action", "ADD")
            order_id = tick["id"]
            tick_side = tick["side"]
            tick_price = tick["price"]
            tick_qty = tick["qty"]

            # Check if incoming market flow fills our resting passive quotes
            if action == "ADD":
                if self.inventory >= self.strategy.max_inventory:
                    active_bid = None
                if self.inventory <= -self.strategy.max_inventory:
                    active_ask = None
                if active_bid and tick_side == Side.SELL and tick_price <= active_bid["price"]:
                    # Our passive bid gets filled!
                    fill_price = active_bid["price"]
                    fill_qty = min(active_bid["qty"], tick_qty, max(0, self.strategy.max_inventory - self.inventory))
                    self.cash -= (fill_price * fill_qty) / 100.0
                    self.inventory += fill_qty
                    self.strategy.inventory = self.inventory
                    self.trades.append({"side": Side.BUY, "price": fill_price, "qty": fill_qty, "arrival_mid": active_bid["mid"], "tick_idx": idx})
                    self.total_volume += fill_qty
                    self.effective_spreads.append(2.0 * (fill_price - active_bid["mid"]))
                    active_bid["qty"] -= fill_qty
                    if active_bid["qty"] == 0:
                        active_bid = None

                if active_ask and tick_side == Side.BUY and tick_price >= active_ask["price"]:
                    # Our passive ask gets filled!
                    fill_price = active_ask["price"]
                    fill_qty = min(active_ask["qty"], tick_qty, max(0, self.strategy.max_inventory + self.inventory))
                    self.cash += (fill_price * fill_qty) / 100.0
                    self.inventory -= fill_qty
                    self.strategy.inventory = self.inventory
                    self.trades.append({"side": Side.SELL, "price": fill_price, "qty": fill_qty, "arrival_mid": active_ask["mid"], "tick_idx": idx})
                    self.total_volume += fill_qty
                    self.effective_spreads.append(2.0 * (active_ask["mid"] - fill_price))
                    active_ask["qty"] -= fill_qty
                    if active_ask["qty"] == 0:
                        active_ask = None

                self.book.add_limit_order(order_id, 1, tick_side, tick_price, tick_qty)
            elif action == "CANCEL":
                self.book.cancel_order(order_id)

            # Strategy decision
            intent = self.strategy.on_tick(self.book)
            if intent:
                action_type = intent["action"]
                qty = intent["qty"]

                bid, ask = self.book.get_bbo()
                if bid and ask:
                    mid = (bid[0] + ask[0]) / 2.0

                    if action_type == "TAKE_BUY" and intent.get("price", 0) >= ask[0]:
                        fill_price = ask[0]
                        self.cash -= (fill_price * qty) / 100.0
                        self.inventory += qty
                        self.strategy.inventory = self.inventory
                        self.trades.append({"side": Side.BUY, "price": fill_price, "qty": qty, "arrival_mid": mid, "tick_idx": idx})
                        self.total_volume += qty
                        self.effective_spreads.append(2.0 * (fill_price - mid))
                    elif action_type == "TAKE_SELL" and intent.get("price", 0) <= bid[0]:
                        fill_price = bid[0]
                        self.cash += (fill_price * qty) / 100.0
                        self.inventory -= qty
                        self.strategy.inventory = self.inventory
                        self.trades.append({"side": Side.SELL, "price": fill_price, "qty": qty, "arrival_mid": mid, "tick_idx": idx})
                        self.total_volume += qty
                        self.effective_spreads.append(2.0 * (mid - fill_price))
                    elif action_type == "QUOTE_BID":
                        active_bid = {"price": intent["price"], "qty": qty, "mid": mid}
                    elif action_type == "QUOTE_ASK":
                        active_ask = {"price": intent["price"], "qty": qty, "mid": mid}
                    elif action_type == "QUOTE_TWO_SIDED":
                        active_bid = {"price": intent["bid_price"], "qty": qty, "mid": mid}
                        active_ask = {"price": intent["ask_price"], "qty": qty, "mid": mid}



            # Evaluate markouts for older trades (e.g. 50 ticks ahead)
            mid, _ = self.book.get_mid_and_spread()
            mids_by_event.append(mid if mid > 0 else None)
            if mid > 0:
                mtm_pnl = self.cash + (self.inventory * mid) / 100.0
                self.pnl_history.append(mtm_pnl)
                peak_pnl = max(peak_pnl, mtm_pnl)
                max_drawdown = max(max_drawdown, peak_pnl - mtm_pnl)

        # Markout TCA calculation
        valid_mids = [m for m in mids_by_event if m is not None]
        last_mid = valid_mids[-1] if valid_mids else 0.0
        for t in self.trades:
            eval_idx = min(len(mids_by_event) - 1, t["tick_idx"] + self.markout_horizon_events)
            post_mid = mids_by_event[eval_idx] if eval_idx >= 0 else None
            if post_mid is None:
                post_mid = last_mid if last_mid > 0 else t["arrival_mid"]
            direction = 1.0 if t["side"] == Side.BUY else -1.0
            realized = 2.0 * direction * (t["price"] - post_mid)
            adverse = -2.0 * direction * (post_mid - t["arrival_mid"])
            self.realized_spreads.append(realized)
            if adverse > 0:
                self.toxic_trades += 1

        final_pnl = self.cash + (self.inventory * last_mid) / 100.0 if last_mid > 0 else self.cash
        
        # Calculate Sharpe Ratio
        if len(self.pnl_history) > 1:
            diffs = [self.pnl_history[i] - self.pnl_history[i - 1] for i in range(1, len(self.pnl_history))]
            mean_ret = sum(diffs) / len(diffs)
            var_ret = sum((r - mean_ret) ** 2 for r in diffs) / len(diffs)
            std_ret = math.sqrt(var_ret) if var_ret > 0 else 1.0
            # Tick spacing is not a fixed wall-clock interval, so annualization would be misleading.
            sharpe = mean_ret / std_ret
        else:
            sharpe = 0.0

        win_trades = sum(1 for r in self.realized_spreads if r >= 0)
        win_rate = (win_trades / len(self.trades)) * 100.0 if self.trades else 0.0

        return BacktestResult(
            total_ticks=len(tick_stream),
            total_trades=len(self.trades),
            total_volume=self.total_volume,
            final_pnl_dollars=final_pnl,
            max_drawdown_dollars=max_drawdown,
            sharpe_ratio=sharpe,
            win_rate=win_rate,
            avg_effective_spread_ticks=sum(self.effective_spreads) / len(self.effective_spreads) if self.effective_spreads else 0.0,
            avg_realized_spread_ticks=sum(self.realized_spreads) / len(self.realized_spreads) if self.realized_spreads else 0.0,
            adverse_selection_ratio=(self.toxic_trades / len(self.trades) * 100.0) if self.trades else 0.0,
            final_inventory=self.inventory
        )
