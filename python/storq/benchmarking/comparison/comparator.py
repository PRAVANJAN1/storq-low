from .regression import detect_regressions, RegressionThresholds
from .comparison_result import BenchmarkComparison


def compare_benchmarks(baseline, current, thresholds=None) -> BenchmarkComparison:
    baseline_config = baseline.configuration
    current_config = current.configuration
    incompatible = []
    for key in ("profile", "concurrency"):
        if baseline_config.get(key) != current_config.get(key):
            incompatible.append(f"{key}: {baseline_config.get(key)!r} != {current_config.get(key)!r}")
    if baseline.environment.get("database_engine") != current.environment.get("database_engine"):
        incompatible.append("database engines differ")
    changes = detect_regressions(baseline, current, RegressionThresholds(thresholds or {}))
    return BenchmarkComparison(
        baseline_id=baseline.benchmark_id,
        current_id=current.benchmark_id,
        comparable=not incompatible,
        incompatibility_reasons=incompatible,
        regressions=changes,
    )
