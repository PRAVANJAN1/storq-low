from dataclasses import dataclass, field
from typing import Any


@dataclass
class BenchmarkComparison:
    baseline_id: str
    current_id: str
    comparable: bool = True
    incompatibility_reasons: list[str] = field(default_factory=list)
    regressions: list[dict[str, Any]] = field(default_factory=list)
