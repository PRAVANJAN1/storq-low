from dataclasses import dataclass, field
from typing import Any


@dataclass
class BenchmarkConfig:
    name: str = "Storq Benchmark"
    database: str | None = None
    profile: str = "mixed"
    seed: int = 1
    iterations: int = 1
    warmup: int = 0
    concurrency: int = 1
    timeout: float = 300
    operation_count: int = 1000
    read_ratio: float = 0.5
    resource_interval: float = 1.0
    workload: dict[str, Any] = field(default_factory=dict)

    def validate(self) -> None:
        if self.iterations < 1:
            raise ValueError("iterations must be at least 1")
        if self.warmup < 0:
            raise ValueError("warmup cannot be negative")
        if self.concurrency < 1:
            raise ValueError("concurrency must be positive")
        if self.timeout <= 0 or self.operation_count < 1:
            raise ValueError("timeout and operation count must be positive")
        if not 0 <= self.read_ratio <= 1:
            raise ValueError("read_ratio must be between zero and one")
