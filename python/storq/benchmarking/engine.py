import asyncio
import random
import time
from collections.abc import Awaitable
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from .config import BenchmarkConfig
from .environment import environment_fingerprint
from .models import BenchmarkMeasurement, BenchmarkResult
from .statistics import latency_stats, mean, coefficient_of_variation


class Benchmark:
    def __init__(self, name: str, database: str | None = None, profile: str = "mixed", seed: int = 1):
        self.name = name
        self.database = database
        self.profile = profile
        self.seed = seed

    async def run(self, iterations=1, warmup=0, operation=None, **kwargs):
        config = BenchmarkConfig(
            name=self.name, database=self.database, profile=self.profile,
            seed=self.seed, iterations=iterations, warmup=warmup, **kwargs)
        return await BenchmarkEngine().run(config, operation=operation)


class BenchmarkEngine:
    async def run(self, config: BenchmarkConfig, operation: Callable[[], Awaitable | object] | None = None) -> BenchmarkResult:
        if isinstance(config, dict):
            config = BenchmarkConfig(**config)
        config.validate()
        if operation is None:
            raise ValueError("a benchmark operation callable is required; workloads must execute real operations")
        started = datetime.now(timezone.utc)
        random.Random(config.seed)
        result = BenchmarkResult(
            name=config.name, profile=config.profile, database=config.database, seed=config.seed,
            start_time=started, warmup_iterations=config.warmup,
            measurement_iterations=config.iterations,
            environment=environment_fingerprint("sqlite" if config.database else None, config.workload),
            configuration={
                "iterations": config.iterations, "warmup": config.warmup,
                "concurrency": config.concurrency, "operation_count": config.operation_count,
                "read_ratio": config.read_ratio, "profile": config.profile,
            },
        )

        async def execute_once(measurement: BenchmarkMeasurement):
            if operation is None:
                await asyncio.sleep(0)
                return
            returned = operation()
            if asyncio.iscoroutine(returned) or isinstance(returned, Awaitable):
                await returned

        for _ in range(config.warmup):
            await asyncio.wait_for(execute_once(BenchmarkMeasurement()), config.timeout)

        for _ in range(config.iterations):
            measurement = BenchmarkMeasurement(operation_count=config.operation_count)
            monotonic_start = time.perf_counter_ns()
            successful = 0
            latencies = []
            try:
                for _ in range(config.operation_count):
                    before = time.perf_counter_ns()
                    try:
                        await asyncio.wait_for(execute_once(measurement), config.timeout)
                        successful += 1
                    except Exception as error:
                        measurement.error = f"{type(error).__name__}: {error}"
                        result.errors.append(measurement.error)
                    finally:
                        latencies.append(time.perf_counter_ns() - before)
            except Exception as error:
                measurement.error = f"{type(error).__name__}: {error}"
                result.errors.append(measurement.error)
            elapsed = (time.perf_counter_ns() - monotonic_start) / 1e9
            measurement.elapsed_seconds = elapsed
            measurement.operation_latencies_ns = latencies
            measurement.successful_operations = successful
            measurement.failed_operations = len(latencies) - successful
            result.measurements.append(measurement)

        elapsed_values = [item.elapsed_seconds for item in result.measurements]
        operation_values = [item.successful_operations for item in result.measurements]
        latencies = [
            latency for measurement in result.measurements
            for latency in measurement.operation_latencies_ns
        ]
        result.aggregate = {
            "latency_ns": latency_stats(latencies),
            "elapsed_seconds": latency_stats(elapsed_values),
            "operations_per_second": [
                count / elapsed if elapsed > 0 else None
                for count, elapsed in zip(operation_values, elapsed_values)
            ],
            "coefficient_of_variation": coefficient_of_variation(elapsed_values),
        }
        result.aggregate["throughput_mean"] = mean(result.aggregate["operations_per_second"])
        result.status = "failed" if result.errors else "completed"
        result.end_time = datetime.now(timezone.utc)
        return result
