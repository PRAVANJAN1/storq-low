import asyncio

import storq
from storq.benchmarking.statistics import latency_stats, percentile, coefficient_of_variation


def test_controlled_percentiles():
    assert percentile([1, 2, 3, 4], 50) == 2.5
    assert latency_stats([])["count"] == 0
    assert coefficient_of_variation([10, 10]) == 0


def test_benchmark_measurement():
    benchmark = storq.Benchmark("controlled", profile="read")
    result = asyncio.run(benchmark.run(iterations=2, warmup=1, operation=lambda: None,
                                       operation_count=3))
    assert result.status == "completed"
    assert len(result.measurements) == 2
    assert result.aggregate["latency_ns"]["count"] == 6


def test_regression_detection():
    baseline = storq.Benchmark("baseline", profile="mixed")
    current = storq.Benchmark("current", profile="mixed")
    async def run(item):
        return await item.run(iterations=1, operation=lambda: None, operation_count=1)
    before = asyncio.run(run(baseline))
    after = asyncio.run(run(current))
    comparison = storq.compare_benchmarks(before, after, {"throughput": -0.05})
    assert comparison.comparable
