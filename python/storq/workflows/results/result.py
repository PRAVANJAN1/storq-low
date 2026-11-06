from dataclasses import dataclass, field
from datetime import datetime
from typing import Any


@dataclass
class StepResult:
    step_id: str
    name: str
    status: str
    output: Any = None
    error: BaseException | None = None
    attempts: int = 1
    started_at: datetime | None = None
    completed_at: datetime | None = None
    duration: float = 0.0

    def to_dict(self) -> dict[str, Any]:
        return {
            "step_id": self.step_id, "name": self.name, "status": self.status,
            "attempts": self.attempts, "duration": self.duration,
            "error": str(self.error) if self.error else None,
        }


@dataclass
class WorkflowResult:
    workflow_id: str
    status: str
    start_time: datetime
    completion_time: datetime
    step_results: dict[str, StepResult] = field(default_factory=dict)
    failed_steps: list[str] = field(default_factory=list)
    skipped_steps: list[str] = field(default_factory=list)
    errors: list[BaseException] = field(default_factory=list)
    outputs: dict[str, Any] = field(default_factory=dict)

    @property
    def duration(self) -> float:
        return max(0.0, (self.completion_time - self.start_time).total_seconds())

    @property
    def completed_steps(self) -> list[str]:
        return [name for name, result in self.step_results.items() if result.status == "completed"]

    def output(self, step_name: str, default=None):
        if step_name not in self.outputs:
            if default is not None:
                return default
            raise KeyError(f"unknown step output: {step_name}")
        return self.outputs[step_name]

    def to_dict(self) -> dict[str, Any]:
        return {
            "workflow_id": self.workflow_id, "status": self.status,
            "start_time": self.start_time.isoformat(),
            "completion_time": self.completion_time.isoformat(),
            "duration": self.duration, "step_results": {
                name: result.to_dict() for name, result in self.step_results.items()
            },
            "failed_steps": self.failed_steps, "skipped_steps": self.skipped_steps,
            "errors": [str(error) for error in self.errors],
        }
