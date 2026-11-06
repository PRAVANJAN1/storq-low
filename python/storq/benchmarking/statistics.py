import math
import statistics


def mean(samples):
    return statistics.fmean(samples) if samples else None


def median(samples):
    return statistics.median(samples) if samples else None


def stddev(samples):
    if len(samples) < 2:
        return 0.0 if samples else None
    return statistics.stdev(samples)


def percentile(samples, percentile_value):
    if not samples:
        return None
    if percentile_value < 0 or percentile_value > 100:
        raise ValueError("percentile must be between 0 and 100")
    ordered = sorted(samples)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentile_value / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def latency_stats(samples):
    return {
        "minimum": min(samples) if samples else None,
        "maximum": max(samples) if samples else None,
        "mean": mean(samples),
        "median": median(samples),
        "stddev": stddev(samples),
        "p50": percentile(samples, 50),
        "p90": percentile(samples, 90),
        "p95": percentile(samples, 95),
        "p99": percentile(samples, 99),
        "count": len(samples),
    }


def coefficient_of_variation(samples):
    average = mean(samples)
    deviation = stddev(samples)
    if average is None or deviation is None or average == 0:
        return None
    return deviation / abs(average)
