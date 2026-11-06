from dataclasses import dataclass
from typing import Mapping

from ..statistics import percentile


@dataclass
class RegressionThresholds:
    thresholds: Mapping[str, float]
    def __init__(self, thresholds=None):
        self.thresholds = dict(thresholds or {})


def _metric(result, name):
    aggregate = result.aggregate
    if name == "throughput" or name == "throughput_mean":
        return aggregate.get("throughput_mean")
    if name in {"p50_latency", "p95_latency", "p99_latency"}:
        key = {"p50_latency": "p50", "p95_latency": "p95", "p99_latency": "p99"}[name]
        return aggregate.get("latency_ns", {}).get(key)
    if name == "peak_memory":
        return result.environment.get("peak_memory_bytes")
    return aggregate.get(name)


def percentage_change(baseline, current):
    if baseline is None or current is None:
        return None
    if baseline == 0:
        return None if current == 0 else float("inf")
    return (current - baseline) / abs(baseline)


def detect_regressions(baseline, current, thresholds: RegressionThresholds) -> list[dict]:
    regressions = []
    for metric, threshold in thresholds.thresholds.items():
        baseline_value = _metric(baseline, "throughput" if metric == "throughput" else metric)
        current_value = _metric(current, "throughput" if metric == "throughput" else metric)
        change = percentage_change(baseline_value, current_value)
        if change is None:
            continue
        latency_metric = metric.endswith("latency") or metric in {"peak_memory", "recovery_time"}
        regressed = change > threshold if latency_metric else change < threshold
        if regressed:
            regressions.append({
                "metric": metric,
                "threshold": threshold,
                "baseline": baseline_value,
                "current": current_value,
                "change": change,
            })
    return regressions
