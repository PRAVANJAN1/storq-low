from abc import ABC, abstractmethod
from typing import Any
from dataclasses import dataclass

from ..execution.state import StepState


@dataclass
class RetryPolicy:
    max_attempts: int = 1
    initial_delay: float = 0.0
    backoff: str = "none"
    max_delay: float | None = None
    retryable_exceptions: tuple[type[BaseException], ...] = (Exception,)
    non_retryable_exceptions: tuple[type[BaseException], ...] = ()

    def validate(self) -> None:
        if self.max_attempts < 1:
            raise ValueError("max_attempts must be at least 1")
        if self.backoff not in {"none", "fixed", "exponential"}:
            raise ValueError("backoff must be none, fixed, or exponential")

    def delay_for_attempt(self, attempt: int) -> float:
        if self.backoff == "exponential":
            value = self.initial_delay * (2 ** max(0, attempt - 1))
        else:
            value = self.initial_delay
        return min(value, self.max_delay) if self.max_delay is not None else value

    def to_dict(self) -> dict[str, Any]:
        return {
            "max_attempts": self.max_attempts,
            "initial_delay": self.initial_delay,
            "backoff": self.backoff,
            "max_delay": self.max_delay,
            "retryable": [item.__name__ for item in self.retryable_exceptions],
            "non_retryable": [item.__name__ for item in self.non_retryable_exceptions],
        }


class WorkflowStep(ABC):
    def __init__(self, name: str, config: dict[str, Any] | None = None, dependencies: list[str] | None = None,
                 timeout: float | None = None, retry: RetryPolicy | None = None, description: str = "",
                 idempotent: bool = True):
        self.name = name
        self.id = name
        self.description = description
        self.config = dict(config or {})
        self.dependencies = list(dependencies or [])
        self.timeout = timeout
        self.retry = retry or RetryPolicy()
        self.state = StepState()
        self.output: Any = None
        self.error: BaseException | None = None
        self.idempotent = idempotent

    @abstractmethod
    async def execute(self, context):
        raise NotImplementedError

    async def validate(self, context) -> bool:
        return True
