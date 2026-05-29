"""
ML Inference Server — FastAPI
Reads features from mmap, runs inference, writes predictions back to mmap.
Also exposes HTTP endpoints for the backend dashboard.
"""

import asyncio
import struct
import time
import os
import json
import logging
from typing import Optional

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from features import (
    SyntheticDataGenerator, ML_FEATURE_NAMES,
    MmapFeatureReader, parse_feature_vector, add_derived_features,
    FEATURE_STRUCT_SIZE
)
from signal_model import SignalModel
from risk_model import RiskModel, generate_regime_labels
from llm_explainer import LLMExplainer

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────
# MLPrediction struct layout (must match C++ types.hpp)
# uint64 timestamp_ns | int8 direction | float confidence
# float fragility_score | bool fragility_alert | char[128] explanation
PREDICT_FORMAT = "<QbffB128s"   # 8+1+4+4+1+128 = 146 bytes
PREDICT_SIZE   = struct.calcsize(PREDICT_FORMAT)

MMAP_FEATURE_PATH  = "/tmp/hft_features.bin"
MMAP_PREDICT_PATH  = "/tmp/hft_predict.bin"
MMAP_SNAPSHOT_PATH = "/tmp/hft_snapshot.bin"


def write_prediction(direction: int, confidence: float,
                     fragility: float, fragility_alert: bool,
                     explanation: str):
    """Write MLPrediction struct to mmap."""
    try:
        exp_bytes = explanation.encode("utf-8")[:127].ljust(128, b"\x00")
        data = struct.pack(PREDICT_FORMAT,
                           int(time.time_ns()),
                           int(direction),
                           float(confidence),
                           float(fragility),
                           int(fragility_alert),
                           exp_bytes)
        # Write to tmp file atomically
        tmp = MMAP_PREDICT_PATH + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, MMAP_PREDICT_PATH)
    except Exception as e:
        logger.error(f"Failed to write prediction: {e}")


# ─────────────────────────────────────────────────────────────
# Pydantic models
# ─────────────────────────────────────────────────────────────
class PredictionResponse(BaseModel):
    direction:        int
    confidence:       float
    regime:           int
    regime_name:      str
    fragility_score:  float
    fragility_alert:  bool
    approved:         bool
    anomaly_score:    float
    explanation:      str
    features:         dict


class HealthResponse(BaseModel):
    status:           str
    signal_trained:   bool
    risk_trained:     bool
    predictions_made: int
    uptime_s:         float


# ─────────────────────────────────────────────────────────────
# App
# ─────────────────────────────────────────────────────────────
app = FastAPI(title="HFT Mesh — ML Inference Server", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Global state
signal_model  = SignalModel()
risk_model    = RiskModel()
llm_explainer = LLMExplainer(api_key=os.environ.get("ANTHROPIC_API_KEY"))
mmap_reader   = MmapFeatureReader(MMAP_FEATURE_PATH)
gen           = SyntheticDataGenerator()  # fallback when C++ engine not running

_start_time       = time.time()
_predictions_made = 0
_latest_prediction: dict = {}
_ws_clients: list = []


# ─────────────────────────────────────────────────────────────
# Startup: train or load models
# ─────────────────────────────────────────────────────────────
@app.on_event("startup")
async def startup():
    global signal_model, risk_model

    logger.info("Loading or training ML models...")

    # Try to load pre-trained models
    if not signal_model.load():
        logger.info("Training signal model from scratch...")
        df, labels = gen.generate_with_labels(50_000)
        signal_model.train(df, labels, verbose=True)

    if not risk_model.load():
        logger.info("Training risk model from scratch...")
        df, _ = gen.generate_with_labels(30_000)
        regime_labels = generate_regime_labels(df)
        risk_model.train(df, regime_labels, verbose=True)

    logger.info("Models ready. Starting inference loop...")
    asyncio.create_task(inference_loop())
    asyncio.create_task(ws_broadcast_loop())


# ─────────────────────────────────────────────────────────────
# Core inference loop
# ─────────────────────────────────────────────────────────────
async def inference_loop():
    """Reads features from mmap every 50ms and runs ML inference."""
    global _predictions_made, _latest_prediction

    while True:
        try:
            # Try mmap first (C++ engine running)
            fv = mmap_reader.read_latest()
            if fv is None:
                # Fallback: generate synthetic tick for demo
                fv = gen.generate()

            # Signal model
            direction, confidence, sig_details = signal_model.predict(fv)

            # Risk model
            risk_result = risk_model.evaluate(fv)
            regime      = risk_result["regime"]
            fragility   = risk_result["fragility_score"]
            approved    = risk_result["approved"]

            # LLM explanation (non-blocking, cached)
            explanation = await llm_explainer.explain(
                fv, direction, confidence, regime, fragility
            )

            # Write prediction to mmap for C++ engine to read
            write_prediction(
                direction if approved else 0,
                confidence,
                fragility,
                risk_result["fragility_alert"],
                explanation
            )

            _predictions_made += 1
            _latest_prediction = {
                "timestamp":       time.time(),
                "direction":       direction,
                "confidence":      round(confidence, 4),
                "regime":          regime,
                "regime_name":     risk_result["regime_name"],
                "fragility_score": round(fragility, 4),
                "fragility_alert": risk_result["fragility_alert"],
                "approved":        approved,
                "anomaly_score":   round(risk_result["anomaly_score"], 6),
                "explanation":     explanation,
                "features":        {k: round(float(fv.get(k, 0)), 6)
                                    for k in ML_FEATURE_NAMES},
                "feature_importance": signal_model.feature_importance(),
                "prob_up":         round(sig_details.get("prob_up", 0), 4),
                "prob_down":       round(sig_details.get("prob_down", 0), 4),
            }

        except Exception as e:
            logger.error(f"Inference loop error: {e}")

        await asyncio.sleep(0.05)  # 50ms polling


# ─────────────────────────────────────────────────────────────
# WebSocket broadcast
# ─────────────────────────────────────────────────────────────
async def ws_broadcast_loop():
    """Push latest prediction to all connected WebSocket clients every 200ms."""
    while True:
        if _latest_prediction and _ws_clients:
            msg = json.dumps(_latest_prediction)
            dead = []
            for ws in _ws_clients:
                try:
                    await ws.send_text(msg)
                except Exception:
                    dead.append(ws)
            for ws in dead:
                _ws_clients.remove(ws)
        await asyncio.sleep(0.2)


@app.websocket("/ws/predictions")
async def ws_predictions(websocket: WebSocket):
    await websocket.accept()
    _ws_clients.append(websocket)
    try:
        while True:
            await websocket.receive_text()  # keep alive
    except WebSocketDisconnect:
        _ws_clients.remove(websocket)


# ─────────────────────────────────────────────────────────────
# HTTP endpoints
# ─────────────────────────────────────────────────────────────
@app.get("/predict", response_model=PredictionResponse)
async def get_prediction():
    """Get the latest ML prediction."""
    if not _latest_prediction:
        return PredictionResponse(
            direction=0, confidence=0, regime=0,
            regime_name="INITIALISING", fragility_score=0,
            fragility_alert=False, approved=True,
            anomaly_score=0, explanation="Initialising...",
            features={}
        )
    p = _latest_prediction
    return PredictionResponse(
        direction       = p["direction"],
        confidence      = p["confidence"],
        regime          = p["regime"],
        regime_name     = p["regime_name"],
        fragility_score = p["fragility_score"],
        fragility_alert = p["fragility_alert"],
        approved        = p["approved"],
        anomaly_score   = p["anomaly_score"],
        explanation     = p["explanation"],
        features        = p["features"],
    )


@app.get("/health", response_model=HealthResponse)
async def health():
    return HealthResponse(
        status           = "ok",
        signal_trained   = signal_model.is_trained,
        risk_trained     = risk_model.is_trained,
        predictions_made = _predictions_made,
        uptime_s         = round(time.time() - _start_time, 1),
    )


@app.get("/feature-importance")
async def feature_importance():
    return {
        "signal": signal_model.feature_importance(),
        "regime": {k: float(v) for k, v in zip(
            ML_FEATURE_NAMES,
            risk_model.regime_model.feature_importances_
        )} if risk_model.is_trained and risk_model.regime_model else {}
    }


@app.get("/llm-stats")
async def llm_stats():
    return llm_explainer.stats()


# ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    uvicorn.run(
        "ml_server:app",
        host="0.0.0.0",
        port=8001,
        reload=False,
        log_level="info",
    )
