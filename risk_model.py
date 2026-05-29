"""
Risk Model — Two components:

1. Autoencoder Anomaly Detector
   Trained on normal P&L / market patterns.
   At inference: measures reconstruction error.
   High error → pattern is unusual → HALT signal.

2. Market Regime Classifier (XGBoost)
   Classifies current market state:
   0 = TRENDING    (price moving directionally)
   1 = MEAN-REVERTING (price oscillating around mean)
   2 = VOLATILE    (high spread, erratic moves)
"""

import numpy as np
import pandas as pd
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, TensorDataset
import xgboost as xgb
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import classification_report
from imblearn.over_sampling import SMOTE
import joblib
import json
import os
import logging
from typing import Tuple, Optional
from features import SyntheticDataGenerator, ML_FEATURE_NAMES, feature_vector_to_array

logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

os.makedirs("models", exist_ok=True)

# ─────────────────────────────────────────────────────────────
# Autoencoder
# ─────────────────────────────────────────────────────────────
class Autoencoder(nn.Module):
    """
    Fully-connected symmetric autoencoder.
    Input dim = 15 (ML features)
    Bottleneck = 4 (latent representation)
    Reconstruction error = anomaly score.
    """
    def __init__(self, input_dim: int = 15, latent_dim: int = 4):
        super().__init__()
        self.encoder = nn.Sequential(
            nn.Linear(input_dim, 32),
            nn.ReLU(),
            nn.Dropout(0.1),
            nn.Linear(32, 16),
            nn.ReLU(),
            nn.Linear(16, latent_dim),
        )
        self.decoder = nn.Sequential(
            nn.Linear(latent_dim, 16),
            nn.ReLU(),
            nn.Linear(16, 32),
            nn.ReLU(),
            nn.Linear(32, input_dim),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        z    = self.encoder(x)
        xhat = self.decoder(z)
        return xhat

    def reconstruction_error(self, x: torch.Tensor) -> torch.Tensor:
        """Mean squared error between input and reconstruction."""
        with torch.no_grad():
            xhat = self.forward(x)
            return torch.mean((x - xhat) ** 2, dim=1)


# ─────────────────────────────────────────────────────────────
class RiskModel:
    """
    Combines autoencoder (anomaly) + XGBoost (regime).
    """
    AE_MODEL_PATH     = "models/autoencoder.pt"
    REGIME_MODEL_PATH = "models/regime_model.joblib"
    SCALER_PATH       = "models/risk_scaler.joblib"
    META_PATH         = "models/risk_meta.json"

    # Threshold above which we issue a HALT signal (tunable)
    ANOMALY_THRESHOLD  = 0.05
    # Flash crash: fragility threshold
    FRAGILITY_THRESHOLD = 0.80

    def __init__(self, input_dim: int = len(ML_FEATURE_NAMES)):
        self.input_dim      = input_dim
        self.ae:             Optional[Autoencoder] = None
        self.regime_model:   Optional[xgb.XGBClassifier] = None
        self.scaler:         Optional[StandardScaler] = None
        self.ae_threshold:   float = self.ANOMALY_THRESHOLD
        self.is_trained:     bool  = False
        self.meta:           dict  = {}

    # ─────────────────────────────────────────────────────────
    # Training
    # ─────────────────────────────────────────────────────────
    def train_autoencoder(self, df_normal: pd.DataFrame,
                           epochs: int = 50, batch_size: int = 256,
                           verbose: bool = True):
        """Train autoencoder on NORMAL samples only."""
        X = df_normal[ML_FEATURE_NAMES].values.astype(np.float32)
        self.scaler = StandardScaler()
        X_scaled = self.scaler.fit_transform(X)

        dataset = TensorDataset(torch.tensor(X_scaled))
        loader  = DataLoader(dataset, batch_size=batch_size, shuffle=True)

        self.ae = Autoencoder(input_dim=self.input_dim)
        optimizer = optim.Adam(self.ae.parameters(), lr=1e-3, weight_decay=1e-5)
        criterion = nn.MSELoss()

        self.ae.train()
        for epoch in range(epochs):
            total_loss = 0.0
            for (batch,) in loader:
                optimizer.zero_grad()
                xhat = self.ae(batch)
                loss = criterion(xhat, batch)
                loss.backward()
                optimizer.step()
                total_loss += loss.item()

            if verbose and (epoch + 1) % 10 == 0:
                avg = total_loss / len(loader)
                logger.info(f"  AE Epoch {epoch+1}/{epochs} — loss={avg:.6f}")

        # Compute threshold = 95th percentile of training reconstruction errors
        self.ae.eval()
        X_t = torch.tensor(X_scaled)
        errors = self.ae.reconstruction_error(X_t).numpy()
        self.ae_threshold = float(np.percentile(errors, 95))
        logger.info(f"AE anomaly threshold set to {self.ae_threshold:.6f}")

    def train_regime_classifier(self, df: pd.DataFrame,
                                  regime_labels: np.ndarray,
                                  verbose: bool = True):
        """
        Train regime classifier.
        Regime labels: 0=trending, 1=mean-reverting, 2=volatile
        """
        X = df[ML_FEATURE_NAMES].values.astype(np.float32)
        X_scaled = self.scaler.transform(X)

        # Handle class imbalance with SMOTE
        try:
            smote = SMOTE(random_state=42, k_neighbors=3)
            X_res, y_res = smote.fit_resample(X_scaled, regime_labels)
            logger.info(f"SMOTE: {len(X_scaled)} → {len(X_res)} samples")
        except Exception:
            X_res, y_res = X_scaled, regime_labels

        self.regime_model = xgb.XGBClassifier(
            n_estimators=200,
            max_depth=4,
            learning_rate=0.05,
            subsample=0.8,
            use_label_encoder=False,
            eval_metric="mlogloss",
            random_state=42,
            n_jobs=-1,
        )
        self.regime_model.fit(X_res, y_res, verbose=False)

        if verbose:
            X_t = self.scaler.transform(df[ML_FEATURE_NAMES].values)
            preds = self.regime_model.predict(X_t)
            print(classification_report(regime_labels, preds,
                  target_names=["TRENDING", "MEAN-REVERTING", "VOLATILE"]))

    def train(self, df: pd.DataFrame, regime_labels: np.ndarray,
              df_anomalous: Optional[pd.DataFrame] = None, verbose: bool = True):
        """Full training pipeline."""
        logger.info("Training autoencoder on normal data...")
        self.train_autoencoder(df, verbose=verbose)

        logger.info("Training regime classifier...")
        self.train_regime_classifier(df, regime_labels, verbose=verbose)

        self.is_trained = True
        self.meta = {
            "ae_threshold":  self.ae_threshold,
            "feature_names": ML_FEATURE_NAMES,
            "n_samples":     len(df),
        }
        self.save()
        logger.info("Risk model training complete.")

    # ─────────────────────────────────────────────────────────
    # Inference
    # ─────────────────────────────────────────────────────────
    def check_anomaly(self, fv: dict) -> Tuple[bool, float]:
        """
        Returns (is_anomalous, anomaly_score).
        is_anomalous = True → HALT order.
        """
        if not self.is_trained or self.ae is None:
            return False, 0.0

        x = feature_vector_to_array(fv).reshape(1, -1).astype(np.float32)
        x_scaled = self.scaler.transform(x)
        x_t = torch.tensor(x_scaled)

        self.ae.eval()
        error = float(self.ae.reconstruction_error(x_t)[0])
        is_anomalous = error > self.ae_threshold
        return is_anomalous, error

    def classify_regime(self, fv: dict) -> Tuple[int, float]:
        """
        Returns (regime_id, confidence).
        regime_id: 0=trending, 1=mean-reverting, 2=volatile
        """
        if not self.is_trained or self.regime_model is None:
            return 0, 0.5

        x = feature_vector_to_array(fv).reshape(1, -1)
        x_scaled = self.scaler.transform(x)
        proba    = self.regime_model.predict_proba(x_scaled)[0]
        regime   = int(np.argmax(proba))
        conf     = float(proba[regime])
        return regime, conf

    def flash_crash_score(self, fv: dict) -> float:
        """
        Compute flash crash fragility probability.
        Uses anomaly score + spread + depth thinning as heuristics.
        Returns 0.0–1.0 (higher = more fragile).
        """
        _, ae_score = self.check_anomaly(fv)

        # Normalise anomaly score to [0,1]
        ae_norm = min(ae_score / (self.ae_threshold * 3), 1.0)

        # High spread → fragile
        spread_norm = min(fv.get("spread_norm", 0) * 100, 1.0)

        # Thin depth → fragile
        depth_total = fv.get("depth_total", 1) or 1
        depth_score = max(0.0, 1.0 - depth_total / 100_000)

        # High cancel rate → fragile
        cancel_rate = min(fv.get("cancel_rate_1s", 0) / 50.0, 1.0)

        # Weighted combination
        fragility = (0.35 * ae_norm +
                     0.25 * spread_norm +
                     0.25 * depth_score +
                     0.15 * cancel_rate)
        return float(np.clip(fragility, 0, 1))

    def evaluate(self, fv: dict) -> dict:
        """
        Full risk evaluation for a single feature vector.
        Returns a dict with all risk signals.
        """
        is_anomalous, ae_score = self.check_anomaly(fv)
        regime, regime_conf    = self.classify_regime(fv)
        fragility              = self.flash_crash_score(fv)

        regime_names = {0: "TRENDING", 1: "MEAN-REVERTING", 2: "VOLATILE"}

        return {
            "approved":        not is_anomalous,
            "anomaly_score":   ae_score,
            "anomaly_threshold": self.ae_threshold,
            "regime":          regime,
            "regime_name":     regime_names.get(regime, "UNKNOWN"),
            "regime_conf":     regime_conf,
            "fragility_score": fragility,
            "fragility_alert": fragility > self.FRAGILITY_THRESHOLD,
        }

    # ─────────────────────────────────────────────────────────
    # Persistence
    # ─────────────────────────────────────────────────────────
    def save(self):
        if self.ae:
            torch.save(self.ae.state_dict(), self.AE_MODEL_PATH)
        if self.regime_model:
            joblib.dump(self.regime_model, self.REGIME_MODEL_PATH)
        if self.scaler:
            joblib.dump(self.scaler, self.SCALER_PATH)
        with open(self.META_PATH, "w") as f:
            json.dump(self.meta, f, indent=2)
        logger.info("Risk model saved.")

    def load(self) -> bool:
        if not all(os.path.exists(p) for p in [
            self.AE_MODEL_PATH, self.REGIME_MODEL_PATH,
            self.SCALER_PATH, self.META_PATH
        ]):
            return False
        try:
            self.scaler = joblib.load(self.SCALER_PATH)
            self.regime_model = joblib.load(self.REGIME_MODEL_PATH)
            with open(self.META_PATH) as f:
                self.meta = json.load(f)
            self.ae_threshold = self.meta.get("ae_threshold", self.ANOMALY_THRESHOLD)

            input_dim = len(self.meta.get("feature_names", ML_FEATURE_NAMES))
            self.ae = Autoencoder(input_dim=input_dim)
            self.ae.load_state_dict(torch.load(self.AE_MODEL_PATH, map_location="cpu"))
            self.ae.eval()
            self.is_trained = True
            logger.info("Risk model loaded.")
            return True
        except Exception as e:
            logger.error(f"Failed to load risk model: {e}")
            return False


# ─────────────────────────────────────────────────────────────
def generate_regime_labels(df: pd.DataFrame) -> np.ndarray:
    """
    Heuristic regime labelling from features:
      VOLATILE (2):       spread_norm > 0.5 AND |return| > 0.001
      MEAN-REVERTING (1): return_divergence < -0.0005 (bouncing back)
      TRENDING (0):       else
    """
    labels = np.zeros(len(df), dtype=int)
    is_volatile = (df["spread_norm"].abs() > 0.001) & \
                  (df["mid_return_10t"].abs() > 0.001)
    is_mean_rev = df["return_divergence"] < -0.0005
    labels[is_volatile] = 2
    labels[~is_volatile & is_mean_rev] = 1
    return labels


# ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print("═══ Training Risk Model ═══")
    gen = SyntheticDataGenerator(seed=123)

    print("Generating training data...")
    df, _ = gen.generate_with_labels(30_000)
    regime_labels = generate_regime_labels(df)

    print(f"Regime distribution: "
          f"TRENDING={sum(regime_labels==0)} "
          f"MEAN-REV={sum(regime_labels==1)} "
          f"VOLATILE={sum(regime_labels==2)}")

    model = RiskModel()
    model.train(df, regime_labels, verbose=True)

    print("\n═══ Inference Test ═══")
    fv = gen.generate()
    result = model.evaluate(fv)
    print(f"Risk evaluation: {json.dumps(result, indent=2)}")
    print("\n✓ Risk model training complete")
