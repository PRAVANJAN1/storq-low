from dataclasses import dataclass, field
from typing import Any


@dataclass
class WorkflowConfig:
    max_concurrency: int = 4
    timeout: float | None = None
    deterministic: bool = False
    runtime: Any = None
    adapter: Any = None
    database_path: str | None = None

    def validate(self) -> None:
        if not isinstance(self.max_concurrency, int) or self.max_concurrency < 1:
            raise ValueError("max_concurrency must be a positive integer")
        if self.timeout is not None and self.timeout < 0:
            raise ValueError("timeout cannot be negative")

    def to_dict(self) -> dict[str, Any]:
        return {
            "max_concurrency": self.max_concurrency,
            "timeout": self.timeout,
            "deterministic": self.deterministic,
            "database_path": self.database_path,
        }
