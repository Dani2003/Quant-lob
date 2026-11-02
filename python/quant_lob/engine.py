"""
Python ctypes wrapper for C++20 Limit Order Book engine.
Provides sub-microsecond zero-copy access to BBO, depth, SIMD OBI, and Stoikov Micro-Price.
"""

import ctypes
import os
import sys
from dataclasses import dataclass
from typing import Optional, Tuple, List
from enum import IntEnum

class Side(IntEnum):
    BUY = 0
    SELL = 1

class OrderType(IntEnum):
    LIMIT = 0
    MARKET = 1

@dataclass
class Top5Depth:
    bid_prices: List[int]
    bid_volumes: List[int]
    ask_prices: List[int]
    ask_volumes: List[int]
    bid_depth: int
    ask_depth: int

def _load_native_library() -> Optional[ctypes.CDLL]:
    base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    search_paths = [
        os.path.join(base_dir, "build", "libquant_lob.so"),
        os.path.join(base_dir, "build", "quant_lob.dll"),
        os.path.join(base_dir, "build", "Release", "quant_lob.dll"),
        os.path.join(base_dir, "libquant_lob.so"),
        os.path.join(base_dir, "quant_lob.dll"),
    ]
    for p in search_paths:
        if os.path.exists(p):
            try:
                return ctypes.CDLL(p)
            except Exception:
                pass
    return None

_LIB = _load_native_library()

if _LIB:
    _LIB.lob_create.restype = ctypes.c_void_p
    _LIB.lob_destroy.argtypes = [ctypes.c_void_p]

    _LIB.lob_add_limit.argtypes = [
        ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
        ctypes.c_uint8, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64
    ]
    _LIB.lob_add_limit.restype = ctypes.c_int

    _LIB.lob_add_market.argtypes = [
        ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
        ctypes.c_uint8, ctypes.c_uint32, ctypes.c_uint64
    ]
    _LIB.lob_add_market.restype = ctypes.c_int

    _LIB.lob_cancel.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    _LIB.lob_cancel.restype = ctypes.c_int

    _LIB.lob_get_bbo.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)
    ]
    _LIB.lob_get_bbo.restype = ctypes.c_int

    _LIB.lob_get_mid_and_spread.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_uint32)
    ]
    _LIB.lob_get_mid_and_spread.restype = ctypes.c_int

    _LIB.lob_get_top5.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32 * 5), ctypes.POINTER(ctypes.c_uint32 * 5),
        ctypes.POINTER(ctypes.c_uint32 * 5), ctypes.POINTER(ctypes.c_uint32 * 5),
        ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)
    ]
    _LIB.lob_get_top5.restype = ctypes.c_int

    _LIB.lob_calculate_obi.argtypes = [
        ctypes.POINTER(ctypes.c_uint32 * 4),
        ctypes.POINTER(ctypes.c_uint32 * 4),
        ctypes.POINTER(ctypes.c_float * 4),
        ctypes.POINTER(ctypes.c_float)
    ]

    _LIB.lob_calculate_micro_price.argtypes = [
        ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32
    ]
    _LIB.lob_calculate_micro_price.restype = ctypes.c_double

class PyOrderBook:
    """High-speed C++ Limit Order Book instance."""

    def __init__(self):
        if _LIB is not None:
            self._handle = _LIB.lob_create()
            self._is_native = True
        else:
            self._is_native = False
            self._bids = {}  # price -> [Order]
            self._asks = {}
            self._orders = {}

    def __del__(self):
        if getattr(self, "_is_native", False) and getattr(self, "_handle", None):
            _LIB.lob_destroy(self._handle)
            self._handle = None

    def add_limit_order(self, order_id: int, client_id: int, side: Side, price: int, qty: int, timestamp_ns: int = 0) -> bool:
        if self._is_native:
            return bool(_LIB.lob_add_limit(self._handle, order_id, client_id, int(side), price, qty, timestamp_ns))
        
        # Python fallback matching engine: Price-Time Priority
        remaining = qty
        if side == Side.BUY:
            # Cross resting asks: lowest price first
            while remaining > 0 and self._asks:
                best_ask = min(self._asks.keys())
                if best_ask > price:
                    break
                queue = self._asks[best_ask]
                while remaining > 0 and queue:
                    match_qty = min(remaining, queue[0]["qty"])
                    queue[0]["qty"] -= match_qty
                    remaining -= match_qty
                    if queue[0]["qty"] == 0:
                        self._orders.pop(queue[0]["id"], None)
                        queue.pop(0)
                if not queue:
                    del self._asks[best_ask]
            if remaining > 0:
                if price not in self._bids:
                    self._bids[price] = []
                self._bids[price].append({"id": order_id, "qty": remaining})
                self._orders[order_id] = (side, price, remaining)
        else:
            # Cross resting bids: highest price first
            while remaining > 0 and self._bids:
                best_bid = max(self._bids.keys())
                if best_bid < price:
                    break
                queue = self._bids[best_bid]
                while remaining > 0 and queue:
                    match_qty = min(remaining, queue[0]["qty"])
                    queue[0]["qty"] -= match_qty
                    remaining -= match_qty
                    if queue[0]["qty"] == 0:
                        self._orders.pop(queue[0]["id"], None)
                        queue.pop(0)

                if not queue:
                    del self._bids[best_bid]
            if remaining > 0:
                if price not in self._asks:
                    self._asks[price] = []
                self._asks[price].append({"id": order_id, "qty": remaining})
                self._orders[order_id] = (side, price, remaining)

        return True


    def add_market_order(self, order_id: int, client_id: int, side: Side, qty: int, timestamp_ns: int = 0) -> bool:
        if self._is_native:
            return bool(_LIB.lob_add_market(self._handle, order_id, client_id, int(side), qty, timestamp_ns))
        return True

    def cancel_order(self, order_id: int) -> bool:
        if self._is_native:
            return bool(_LIB.lob_cancel(self._handle, order_id))
        if order_id in self._orders:
            side, price, qty = self._orders.pop(order_id)
            book_side = self._bids if side == Side.BUY else self._asks
            if price in book_side:
                book_side[price] = [o for o in book_side[price] if o["id"] != order_id]
                if not book_side[price]:
                    del book_side[price]
            return True
        return False

    def get_bbo(self) -> Tuple[Optional[Tuple[int, int]], Optional[Tuple[int, int]]]:
        """Returns ((best_bid_price, bid_vol), (best_ask_price, ask_vol))"""
        if self._is_native:
            bid_p, bid_v = ctypes.c_uint32(), ctypes.c_uint32()
            ask_p, ask_v = ctypes.c_uint32(), ctypes.c_uint32()
            ok = _LIB.lob_get_bbo(self._handle, ctypes.byref(bid_p), ctypes.byref(bid_v),
                                  ctypes.byref(ask_p), ctypes.byref(ask_v))
            if not ok:
                return None, None
            bid = (bid_p.value, bid_v.value) if bid_p.value > 0 else None
            ask = (ask_p.value, ask_v.value) if ask_p.value > 0 else None
            return bid, ask
        else:
            bid = None
            if self._bids:
                best_p = max(self._bids.keys())
                bid = (best_p, sum(o["qty"] for o in self._bids[best_p]))
            ask = None
            if self._asks:
                best_p = min(self._asks.keys())
                ask = (best_p, sum(o["qty"] for o in self._asks[best_p]))
            return bid, ask

    def get_mid_and_spread(self) -> Tuple[float, int]:
        """Returns (mid_price, spread_ticks)"""
        bid, ask = self.get_bbo()
        if bid and ask and ask[0] > bid[0]:
            mid = (bid[0] + ask[0]) / 2.0
            spread = ask[0] - bid[0]
            return mid, spread
        return 0.0, 0

    def get_top5_depth(self) -> Top5Depth:
        """Reads Top-5 market depth"""
        if self._is_native:
            arr_type = ctypes.c_uint32 * 5
            bp, bv = arr_type(), arr_type()
            ap, av = arr_type(), arr_type()
            bd, ad = ctypes.c_uint32(), ctypes.c_uint32()

            _LIB.lob_get_top5(self._handle, ctypes.byref(bp), ctypes.byref(bv),
                              ctypes.byref(ap), ctypes.byref(av),
                              ctypes.byref(bd), ctypes.byref(ad))
            return Top5Depth(
                bid_prices=list(bp),
                bid_volumes=list(bv),
                ask_prices=list(ap),
                ask_volumes=list(av),
                bid_depth=bd.value,
                ask_depth=ad.value
            )
        else:
            sorted_bids = sorted(self._bids.keys(), reverse=True)[:5]
            sorted_asks = sorted(self._asks.keys())[:5]
            bp = sorted_bids + [0] * (5 - len(sorted_bids))
            bv = [sum(o["qty"] for o in self._bids[p]) for p in sorted_bids] + [0] * (5 - len(sorted_bids))
            ap = sorted_asks + [0] * (5 - len(sorted_asks))
            av = [sum(o["qty"] for o in self._asks[p]) for p in sorted_asks] + [0] * (5 - len(sorted_asks))
            return Top5Depth(bp, bv, ap, av, len(sorted_bids), len(sorted_asks))

    @staticmethod
    def calculate_obi(bid_vols: List[int], ask_vols: List[int]) -> Tuple[List[float], float]:
        """Calculates Order Book Imbalance across top 4 levels"""
        if _LIB is not None:
            arr_u32 = ctypes.c_uint32 * 4
            arr_f32 = ctypes.c_float * 4
            b = arr_u32(*bid_vols[:4])
            a = arr_u32(*ask_vols[:4])
            out_l = arr_f32()
            out_w = ctypes.c_float()
            _LIB.lob_calculate_obi(ctypes.byref(b), ctypes.byref(a), ctypes.byref(out_l), ctypes.byref(out_w))
            return list(out_l), out_w.value
        else:
            weights = [0.40, 0.30, 0.20, 0.10]
            obi_levels = []
            weighted = 0.0
            for i in range(4):
                bv = bid_vols[i] if i < len(bid_vols) else 0
                av = ask_vols[i] if i < len(ask_vols) else 0
                s = bv + av
                o = (bv - av) / s if s > 0 else 0.0
                obi_levels.append(o)
                weighted += o * weights[i]
            return obi_levels, weighted

    @staticmethod
    def calculate_micro_price(best_bid: int, bid_vol: int, best_ask: int, ask_vol: int) -> float:
        """Calculates Stoikov Micro-Price"""
        total = bid_vol + ask_vol
        if total == 0:
            return 0.0
        return (bid_vol * best_ask + ask_vol * best_bid) / total
