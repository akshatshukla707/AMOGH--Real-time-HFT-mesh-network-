"""
LLM Signal Explainer
Uses Claude API to generate plain-English market commentary
from ML predictions and order book features.

Updates every 1 second. Cached to avoid blocking the UI.
Falls back to rule-based explanation if API is slow/unavailable.
"""

import asyncio
import anthropic
import json
import time
import logging
from typing import Optional
from features import ML_FEATURE_NAMES

logger = logging.getLogger(__name__)

SYSTEM_PROMPT = """You are a real-time market analyst explaining HFT order book conditions to retail investors.

Rules:
- Write EXACTLY one sentence (maximum 20 words)
- Plain English only — no technical jargon
- Describe what the market is doing RIGHT NOW
- Be specific about direction and momentum when clear
- Never mention "HFT", "basis points", "ticks", "order flow", "imbalance"
- Examples of good output:
  "Bitcoin buyers are outnumbering sellers, suggesting a possible short-term price rise."
  "Market is unusually quiet with thin trading volume — expect a sudden move soon."
  "Prices on two exchanges briefly diverged before quickly equalising."
  "Strong selling pressure is building; price likely to dip in the next few seconds."
"""

# Rule-based fallback (no API needed)
def rule_based_explanation(fv: dict, direction: int, confidence: float,
                            regime: int, fragility: float) -> str:
    regime_names = {0: "trending steadily", 1: "oscillating", 2: "moving erratically"}
    regime_desc  = regime_names.get(regime, "active")

    if fragility > 0.80:
        return "Market conditions are unusually fragile — a sudden large price move may be imminent."

    if direction == 1 and confidence > 0.70:
        imb = fv.get("imbalance_l1", 0)
        if imb > 0.3:
            return "More buyers than sellers in the order book — Bitcoin price likely rising shortly."
        return "Signals suggest short-term upward price pressure is building."

    if direction == -1 and confidence > 0.70:
        return "Selling orders are outweighing buyers — a brief price dip appears likely."

    arb = fv.get("arb_gap_bps", 0)
    if arb > 2:
        return "A small price gap between exchanges was detected and is being closed by traders."

    if regime == 2:
        return "Bitcoin is trading erratically with wide price swings — caution advised."
    if regime == 1:
        return f"The market is {regime_desc} around its current price level."

    return "Market conditions appear normal with balanced buying and selling activity."


class LLMExplainer:
    """
    Async LLM-powered signal explainer.
    Calls Claude API with latest features + ML predictions.
    Caches the last successful explanation.
    Max 1 API call per second.
    """

    def __init__(self, api_key: Optional[str] = None,
                 min_interval_s: float = 1.0):
        self.client         = anthropic.Anthropic(api_key=api_key) if api_key else None
        self.min_interval   = min_interval_s
        self._last_call_ts  = 0.0
        self._cached_text   = "Initialising market analysis..."
        self._call_count    = 0
        self._error_count   = 0

    async def explain(self, fv: dict, direction: int, confidence: float,
                      regime: int, fragility: float) -> str:
        """
        Generate a plain-English explanation of current market conditions.
        Returns cached result if called too frequently.
        """
        now = time.time()
        if now - self._last_call_ts < self.min_interval:
            return self._cached_text

        self._last_call_ts = now

        if self.client is None:
            # No API key — use rule-based fallback
            self._cached_text = rule_based_explanation(
                fv, direction, confidence, regime, fragility)
            return self._cached_text

        try:
            explanation = await self._call_claude(fv, direction, confidence,
                                                   regime, fragility)
            self._cached_text = explanation
            self._call_count += 1
            return explanation

        except Exception as e:
            self._error_count += 1
            logger.warning(f"LLM API error (#{self._error_count}): {e}")
            # Fall back to rule-based
            self._cached_text = rule_based_explanation(
                fv, direction, confidence, regime, fragility)
            return self._cached_text

    async def _call_claude(self, fv: dict, direction: int, confidence: float,
                            regime: int, fragility: float) -> str:
        regime_names = {0: "trending", 1: "mean-reverting", 2: "volatile"}

        user_content = f"""Current market data:
- Price direction prediction: {"UP" if direction==1 else "DOWN" if direction==-1 else "NEUTRAL"} (confidence: {confidence:.0%})
- Market regime: {regime_names.get(regime, "unknown")}
- Order book imbalance: {fv.get('imbalance_l1', 0):+.2f} (positive = more buyers)
- Bid-ask spread: {fv.get('bid_ask_spread', 0)} ticks
- Cross-exchange price gap: {fv.get('arb_gap_bps', 0):.1f} basis points
- Flash crash risk: {fragility:.0%}
- Recent price return (10 ticks): {fv.get('mid_return_10t', 0):+.4%}

Write one sentence (max 20 words) describing current conditions for a retail investor."""

        # Run synchronous Anthropic client in thread pool
        loop = asyncio.get_event_loop()
        response = await loop.run_in_executor(
            None,
            lambda: self.client.messages.create(
                model="claude-sonnet-4-20250514",
                max_tokens=60,
                system=SYSTEM_PROMPT,
                messages=[{"role": "user", "content": user_content}]
            )
        )
        text = response.content[0].text.strip()
        # Ensure it ends with a period
        if text and not text[-1] in ".!?":
            text += "."
        return text

    def stats(self) -> dict:
        return {
            "api_calls":    self._call_count,
            "api_errors":   self._error_count,
            "cached_text":  self._cached_text,
        }


# ─────────────────────────────────────────────────────────────
# Standalone test
# ─────────────────────────────────────────────────────────────
async def test_explainer():
    from features import SyntheticDataGenerator, add_derived_features
    gen = SyntheticDataGenerator()
    fv  = gen.generate()

    print("Testing rule-based fallback (no API key)...")
    explainer = LLMExplainer(api_key=None)

    for direction, confidence in [(1, 0.82), (-1, 0.71), (0, 0.45)]:
        text = await explainer.explain(fv, direction, confidence, 0, 0.2)
        print(f"  [{'+1 UP' if direction==1 else '-1 DN' if direction==-1 else ' 0 NE'}] {text}")

    # High fragility
    fv2 = gen.generate()
    text = await explainer.explain(fv2, 0, 0.5, 2, 0.92)
    print(f"  [FRAGILE]  {text}")

    print("\n✓ LLM explainer OK")


if __name__ == "__main__":
    asyncio.run(test_explainer())
