"""
Feature Engineering Pipeline
Reads FeatureVector structs from mmap (written by C++ engine)
and prepares them for ML model inference.

FeatureVector C++ layout (mapped exactly):
  timestamp_ns       : uint64  (8 bytes)
  symbol_id          : uint32  (4 bytes)
  bid_ask_spread     : int32   (4 bytes)
  mid_price          : int64   (8 bytes)
  imbalance_l1       : float32 (4 bytes)
  imbalance_l5       : float32 (4 bytes)
  total_bid_depth    : int64   (8 bytes)
  total_ask_depth    : int64   (8 bytes)
  cancel_rate_1s     : float32 (4 bytes)
  trade_intensity_1s : float32 (4 bytes)
  mid_return_10t     : float32 (4 bytes)
  mid_return_50t     : float32 (4 bytes)
  arb_gap_bps        : float32 (4 bytes)
  Total              : 72 bytes
"""

import struct
import mmap
import numpy as np
import pandas as pd
from collections import deque
from typing import Optional, Tuple
import time
import logging

logger = logging.getLogger(__name__)

# C++ FeatureVector struct format (little-endian)
FEATURE_STRUCT_FORMAT = "<QIiqfffqqfffff"  # 72 bytes
FEATURE_STRUCT_SIZE   = struct.calcsize(FEATURE_STRUCT_FORMAT)
assert FEATURE_STRUCT_SIZE == 72, f"Expected 72 bytes, got {FEATURE_STRUCT_SIZE}"

FEATURE_NAMES = [
    "timestamp_ns",
    "symbol_id",
    "bid_ask_spread",
    "mid_price",
    "imbalance_l1",
    "imbalance_l5",
    "total_bid_depth",
    "total_ask_depth",
    "cancel_rate_1s",
    "trade_intensity_1s",
    "mid_return_10t",
    "mid_return_50t",
    "arb_gap_bps",
]

# Features used for ML models (exclude timestamp and symbol_id)
ML_FEATURE_NAMES = [
    "bid_ask_spread",
    "imbalance_l1",
    "imbalance_l5",
    "total_bid_depth",
    "total_ask_depth",
    "cancel_rate_1s",
    "trade_intensity_1s",
    "mid_return_10t",
    "mid_return_50t",
    "arb_gap_bps",
    # Derived features:
    "depth_ratio",          # bid_depth / ask_depth
    "spread_norm",          # spread / mid_price
    "imbalance_diff",       # l1 - l5
    "depth_total",          # bid + ask depth
    "return_divergence",    # mid_return_10t - mid_return_50t
]


def parse_feature_vector(raw_bytes: bytes) -> Optional[dict]:
    """Parse raw bytes from mmap into a feature dict."""
    if len(raw_bytes) < FEATURE_STRUCT_SIZE:
        return None
    try:
        values = struct.unpack(FEATURE_STRUCT_FORMAT, raw_bytes[:FEATURE_STRUCT_SIZE])
        fv = dict(zip(FEATURE_NAMES, values))
        return fv
    except struct.error as e:
        logger.error(f"Failed to parse FeatureVector: {e}")
        return None


def add_derived_features(fv: dict) -> dict:
    """Add engineered features derived from raw order book data."""
    mid = fv.get("mid_price", 1) or 1
    bid_d = fv.get("total_bid_depth", 0)
    ask_d = fv.get("total_ask_depth", 0)
    total_d = bid_d + ask_d

    fv["depth_ratio"]      = bid_d / ask_d if ask_d > 0 else 1.0
    fv["spread_norm"]      = fv.get("bid_ask_spread", 0) / mid if mid > 0 else 0.0
    fv["imbalance_diff"]   = fv.get("imbalance_l1", 0) - fv.get("imbalance_l5", 0)
    fv["depth_total"]      = float(total_d)
    fv["return_divergence"]= fv.get("mid_return_10t", 0) - fv.get("mid_return_50t", 0)
    return fv


def feature_vector_to_array(fv: dict) -> np.ndarray:
    """Convert feature dict → numpy array in ML_FEATURE_NAMES order."""
    return np.array([fv.get(f, 0.0) for f in ML_FEATURE_NAMES], dtype=np.float32)


class FeatureBuffer:
    """
    Maintains a rolling window of feature vectors.
    Used for sequence models (LSTM) and time-series statistics.
    """
    def __init__(self, window_size: int = 100):
        self.window_size = window_size
        self._buf: deque = deque(maxlen=window_size)

    def push(self, fv: dict):
        self._buf.append(fv)

    def latest(self) -> Optional[dict]:
        return self._buf[-1] if self._buf else None

    def to_array(self) -> np.ndarray:
        """Return (T, F) shaped array for the rolling window."""
        rows = [feature_vector_to_array(fv) for fv in self._buf]
        if not rows:
            return np.zeros((0, len(ML_FEATURE_NAMES)), dtype=np.float32)
        return np.stack(rows, axis=0)

    def is_full(self) -> bool:
        return len(self._buf) == self.window_size

    def __len__(self):
        return len(self._buf)


class MmapFeatureReader:
    """
    Reads FeatureVector structs from the mmap file written by the C++ engine.
    Polls every poll_interval_ms milliseconds.
    """
    def __init__(self, path: str = "/tmp/hft_features.bin",
                 poll_interval_ms: float = 10.0):
        self.path             = path
        self.poll_interval    = poll_interval_ms / 1000.0
        self._last_timestamp  = 0
        self._buf             = FeatureBuffer(window_size=100)

    def read_latest(self) -> Optional[dict]:
        """Read the latest feature vector from mmap. Returns None if stale."""
        try:
            with open(self.path, "rb") as f:
                raw = f.read(FEATURE_STRUCT_SIZE)
            if len(raw) < FEATURE_STRUCT_SIZE:
                return None
            fv = parse_feature_vector(raw)
            if fv is None:
                return None
            # Skip if same timestamp as last read
            if fv["timestamp_ns"] == self._last_timestamp:
                return None
            self._last_timestamp = fv["timestamp_ns"]
            fv = add_derived_features(fv)
            self._buf.push(fv)
            return fv
        except (FileNotFoundError, OSError):
            return None

    def get_feature_buffer(self) -> FeatureBuffer:
        return self._buf


class SyntheticDataGenerator:
    """
    Generates synthetic order book feature vectors for development/testing
    when the C++ engine is not running.
    """
    def __init__(self, seed: int = 42):
        self.rng = np.random.default_rng(seed)
        self._mid_price = 4_500_000  # BTC @ $45,000 in ticks (2 decimal places)
        self._t = 0

    def generate(self) -> dict:
        """Generate one realistic synthetic feature vector."""
        self._t += 1
        # Simulate random walk for mid price
        self._mid_price += int(self.rng.normal(0, 100))

        imbalance_l1 = float(np.clip(self.rng.normal(0, 0.3), -1, 1))
        imbalance_l5 = float(np.clip(self.rng.normal(imbalance_l1 * 0.7, 0.2), -1, 1))

        fv = {
            "timestamp_ns":       int(time.time_ns()),
            "symbol_id":          0,
            "bid_ask_spread":     int(self.rng.integers(1, 10)),
            "mid_price":          self._mid_price,
            "imbalance_l1":       imbalance_l1,
            "imbalance_l5":       imbalance_l5,
            "total_bid_depth":    int(self.rng.integers(1000, 50000)),
            "total_ask_depth":    int(self.rng.integers(1000, 50000)),
            "cancel_rate_1s":     float(self.rng.exponential(5)),
            "trade_intensity_1s": float(self.rng.exponential(2)),
            "mid_return_10t":     float(self.rng.normal(0, 0.001)),
            "mid_return_50t":     float(self.rng.normal(0, 0.003)),
            "arb_gap_bps":        float(max(0, self.rng.normal(0.5, 1.0))),
        }
        return add_derived_features(fv)

    def generate_batch(self, n: int = 10000) -> pd.DataFrame:
        """Generate n samples for training."""
        rows = [self.generate() for _ in range(n)]
        return pd.DataFrame(rows)

    def generate_with_labels(self, n: int = 10000) -> Tuple[pd.DataFrame, np.ndarray]:
        """
        Generate n samples with price direction labels.
        Label = 1 if next price goes up, -1 if down, 0 if neutral.
        Simulate: high positive imbalance → price likely goes UP.
        """
        rows = [self.generate() for _ in range(n)]
        df = pd.DataFrame(rows)

        # Realistic labelling: price direction correlated with order flow imbalance
        prob_up = 0.5 + 0.4 * df["imbalance_l1"]  # [-0.1, 0.9]
        prob_up = prob_up.clip(0.05, 0.95)
        noise   = np.random.normal(0, 0.1, n)
        score   = prob_up + noise

        labels = np.where(score > 0.6,   1,   # UP
                 np.where(score < 0.4,  -1,   # DOWN
                                         0))  # NEUTRAL
        return df, labels


if __name__ == "__main__":
    print("Feature Engineering Pipeline — Self-test")
    gen = SyntheticDataGenerator()
    df, labels = gen.generate_with_labels(1000)
    print(f"Generated {len(df)} samples")
    print(f"Label distribution: UP={sum(labels==1)} DOWN={sum(labels==-1)} NEUTRAL={sum(labels==0)}")
    print(f"\nFeature sample:\n{df[ML_FEATURE_NAMES].iloc[0]}")
    print("\n✓ Feature engineering pipeline OK")
