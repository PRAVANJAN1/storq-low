from dataclasses import dataclass, field
from datetime import datetime
from typing import Any
import uuid


@dataclass
class BenchmarkMeasurement:
    operation_latencies_ns: list[float] = field(default_factory=list)
    transaction_latencies_ns: list[float] = field(default_factory=list)
    elapsed_seconds: float = 0.0
    operation_count: int = 0
    successful_operations: int = 0
    failed_operations: int = 0
    transaction_count: int = 0
    committed_transactions: int = 0
    retries: int = 0
    conflicts: int = 0
    error: str | None = None


@dataclass
class BenchmarkResult:
    benchmark_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    name: str = ""
    status: str = "created"
    profile: str = "mixed"
    database: str | None = None
    seed: int = 1
    start_time: datetime | None = None
    end_time: datetime | None = None
    warmup_iterations: int = 0
    measurement_iterations: int = 0
    environment: dict[str, Any] = field(default_factory=dict)
    configuration: dict[str, Any] = field(default_factory=dict)
    measurements: list[BenchmarkMeasurement] = field(default_factory=list)
    aggregate: dict[str, Any] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)

