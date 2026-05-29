"""
Signal Model — Price Direction Predictor
Uses XGBoost to predict short-term price movement direction
from order book features.

Training: Synthetic data (or LOBster dataset in production)
Inference: < 2ms per prediction
Output: direction (-1/0/+1) + confidence (0.0–1.0)
"""

import numpy as np
import pandas as pd
import xgboost as xgb
from sklearn.model_selection import TimeSeriesSplit
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import classification_report, roc_auc_score
import joblib
import json
import os
import logging
from typing import Tuple, Optional
from features import (
    SyntheticDataGenerator, ML_FEATURE_NAMES,
    feature_vector_to_array, FeatureBuffer
)

logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

MODEL_PATH  = "models/signal_model.joblib"
SCALER_PATH = "models/signal_scaler.joblib"
META_PATH   = "models/signal_meta.json"
os.makedirs("models", exist_ok=True)


class SignalModel:
    """
    XGBoost-based price direction classifier.

    Three classes:
        +1  = price will go UP   in next 100ms
         0  = NEUTRAL (< threshold move)
        -1  = price will go DOWN in next 100ms

    Strategy: train binary UP vs REST, DOWN vs REST;
    take the class with highest probability if above threshold.
    Threshold = 0.55 to filter noise (NEUTRAL zone).
    """

    CONFIDENCE_THRESHOLD = 0.55
    NEUTRAL_ZONE         = 0.10  # probability band around 0.5 → NEUTRAL

    def __init__(self):
        self.model_up:   Optional[xgb.XGBClassifier] = None
        self.model_down: Optional[xgb.XGBClassifier] = None
        self.scaler:     Optional[StandardScaler]     = None
        self.is_trained: bool = False
        self.meta:       dict = {}

    # ─────────────────────────────────────────────────────────
    # Training
    # ─────────────────────────────────────────────────────────
    def train(self, df: pd.DataFrame, labels: np.ndarray,
              n_splits: int = 5, verbose: bool = True) -> dict:
        """
        Train the signal model on feature data with direction labels.

        Args:
            df:      DataFrame with ML_FEATURE_NAMES columns
            labels:  np.array of -1, 0, +1 labels
            n_splits: TimeSeriesSplit folds (chronological — never shuffle!)
            verbose: print training progress

        Returns:
            dict with train/val metrics
        """
        X = df[ML_FEATURE_NAMES].values.astype(np.float32)
        y = labels

        # Scale features
        self.scaler = StandardScaler()
        X_scaled = self.scaler.fit_transform(X)

        # Binary labels: UP vs REST, DOWN vs REST
        y_up   = (y == 1).astype(int)
        y_down = (y == -1).astype(int)

        # XGBoost params optimised for tabular financial data
        xgb_params = dict(
            n_estimators=300,
            max_depth=5,
            learning_rate=0.05,
            subsample=0.8,
            colsample_bytree=0.8,
            min_child_weight=10,      # regularise: avoid fitting noise
            reg_alpha=0.1,
            reg_lambda=1.0,
            scale_pos_weight=1.0,
            eval_metric="logloss",
            use_label_encoder=False,
            random_state=42,
            n_jobs=-1,
            tree_method="hist",       # fast histogram algorithm
        )

        # TimeSeriesSplit: NEVER shuffle financial time series
        tscv = TimeSeriesSplit(n_splits=n_splits)
        splits = list(tscv.split(X_scaled))
        train_idx, val_idx = splits[-1]  # use last fold for final eval

        X_train, X_val = X_scaled[train_idx], X_scaled[val_idx]
        y_up_train, y_up_val     = y_up[train_idx],   y_up[val_idx]
        y_down_train, y_down_val = y_down[train_idx], y_down[val_idx]

        if verbose:
            logger.info(f"Training signal model on {len(train_idx)} samples, "
                        f"validating on {len(val_idx)}")

        # Train UP classifier
        self.model_up = xgb.XGBClassifier(**xgb_params)
        self.model_up.fit(
            X_train, y_up_train,
            eval_set=[(X_val, y_up_val)],
            verbose=False,
        )

        # Train DOWN classifier
        self.model_down = xgb.XGBClassifier(**xgb_params)
        self.model_down.fit(
            X_train, y_down_train,
            eval_set=[(X_val, y_down_val)],
            verbose=False,
        )

        # Evaluate
        metrics = self._evaluate(X_val, y_up_val, y_down_val, y[val_idx], verbose)
        self.is_trained = True

        # Feature importance
        self.meta = {
            "feature_names":    ML_FEATURE_NAMES,
            "n_train_samples":  int(len(train_idx)),
            "n_val_samples":    int(len(val_idx)),
            "metrics":          metrics,
            "feature_importance": dict(zip(
                ML_FEATURE_NAMES,
                self.model_up.feature_importances_.tolist()
            )),
        }

        self.save()
        if verbose:
            logger.info(f"Training complete. Metrics: {metrics}")
        return metrics

    def _evaluate(self, X_val, y_up_val, y_down_val, y_true, verbose: bool) -> dict:
        prob_up   = self.model_up.predict_proba(X_val)[:, 1]
        prob_down = self.model_down.predict_proba(X_val)[:, 1]
        preds     = self._combine_predictions(prob_up, prob_down)

        from sklearn.metrics import accuracy_score
        acc = float(accuracy_score(y_true, preds))

        try:
            auc_up   = float(roc_auc_score(y_up_val, prob_up))
            auc_down = float(roc_auc_score(y_down_val, prob_down))
        except Exception:
            auc_up = auc_down = 0.5

        if verbose:
            print(classification_report(y_true, preds, labels=[-1, 0, 1],
                  target_names=["DOWN", "NEUTRAL", "UP"]))

        return {"accuracy": acc, "auc_up": auc_up, "auc_down": auc_down}

    # ─────────────────────────────────────────────────────────
    # Inference
    # ─────────────────────────────────────────────────────────
    def predict(self, fv: dict) -> Tuple[int, float, dict]:
        """
        Predict price direction from a single feature vector.

        Returns:
            direction:   +1 (UP), -1 (DOWN), 0 (NEUTRAL/HOLD)
            confidence:  probability of the predicted direction (0.0–1.0)
            details:     prob_up, prob_down for dashboard display
        """
        if not self.is_trained:
            return 0, 0.0, {}

        x = feature_vector_to_array(fv).reshape(1, -1)
        x_scaled = self.scaler.transform(x)

        prob_up   = float(self.model_up.predict_proba(x_scaled)[0, 1])
        prob_down = float(self.model_down.predict_proba(x_scaled)[0, 1])
        direction, confidence = self._direction_confidence(prob_up, prob_down)

        return direction, confidence, {"prob_up": prob_up, "prob_down": prob_down}

    def _combine_predictions(self, prob_up: np.ndarray,
                              prob_down: np.ndarray) -> np.ndarray:
        """Combine UP/DOWN probabilities into -1/0/+1 labels."""
        results = np.zeros(len(prob_up), dtype=int)
        results[prob_up > self.CONFIDENCE_THRESHOLD]   =  1
        results[prob_down > self.CONFIDENCE_THRESHOLD] = -1
        # Both high → NEUTRAL (conflicting signal)
        conflicted = (prob_up > 0.5) & (prob_down > 0.5)
        results[conflicted] = 0
        return results

    def _direction_confidence(self, prob_up: float,
                               prob_down: float) -> Tuple[int, float]:
        if prob_up > prob_down and prob_up > self.CONFIDENCE_THRESHOLD:
            return +1, prob_up
        elif prob_down > prob_up and prob_down > self.CONFIDENCE_THRESHOLD:
            return -1, prob_down
        return 0, max(prob_up, prob_down)

    def feature_importance(self) -> dict:
        if not self.is_trained:
            return {}
        return dict(zip(ML_FEATURE_NAMES,
                        self.model_up.feature_importances_.tolist()))

    # ─────────────────────────────────────────────────────────
    # Persistence
    # ─────────────────────────────────────────────────────────
    def save(self):
        joblib.dump({"model_up": self.model_up, "model_down": self.model_down},
                    MODEL_PATH)
        joblib.dump(self.scaler, SCALER_PATH)
        with open(META_PATH, "w") as f:
            json.dump(self.meta, f, indent=2)
        logger.info(f"Signal model saved to {MODEL_PATH}")

    def load(self) -> bool:
        if not os.path.exists(MODEL_PATH):
            return False
        try:
            models = joblib.load(MODEL_PATH)
            self.model_up   = models["model_up"]
            self.model_down = models["model_down"]
            self.scaler     = joblib.load(SCALER_PATH)
            with open(META_PATH) as f:
                self.meta = json.load(f)
            self.is_trained = True
            logger.info(f"Signal model loaded. Metrics: {self.meta.get('metrics')}")
            return True
        except Exception as e:
            logger.error(f"Failed to load signal model: {e}")
            return False


# ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print("═══ Training Signal Model ═══")
    gen = SyntheticDataGenerator(seed=42)

    print("Generating training data...")
    df_train, labels_train = gen.generate_with_labels(50_000)
    print(f"Dataset: {len(df_train)} samples | "
          f"UP={sum(labels_train==1)} "
          f"DOWN={sum(labels_train==-1)} "
          f"NEUTRAL={sum(labels_train==0)}")

    model = SignalModel()
    metrics = model.train(df_train, labels_train, verbose=True)

    print("\n═══ Inference Test ═══")
    fv = gen.generate()
    direction, conf, details = model.predict(fv)
    print(f"Direction: {'+1 UP' if direction==1 else '-1 DOWN' if direction==-1 else '0 NEUTRAL'}")
    print(f"Confidence: {conf:.3f}")
    print(f"Details: {details}")
    print("\n✓ Signal model training complete")
