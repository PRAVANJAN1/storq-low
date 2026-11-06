from enum import Enum
from dataclasses import dataclass, field
from datetime import datetime, timezone


def _now() -> datetime:
    return datetime.now(timezone.utc)


class WorkflowStatus(str, Enum):
    CREATED = "created"
    VALIDATING = "validating"
    READY = "ready"
    RUNNING = "running"
    PAUSED = "paused"
    CANCELLING = "cancelling"
    CANCELLED = "cancelled"
    COMPLETED = "completed"
    FAILED = "failed"
    PARTIALLY_COMPLETED = "partially_completed"


class StepStatus(str, Enum):
    PENDING = "pending"
    READY = "ready"
    RUNNING = "running"
    RETRYING = "retrying"
    COMPLETED = "completed"
    FAILED = "failed"
    SKIPPED = "skipped"
    CANCELLED = "cancelled"


_ALLOWED = {
    WorkflowStatus.CREATED: {WorkflowStatus.VALIDATING},
    WorkflowStatus.VALIDATING: {WorkflowStatus.READY, WorkflowStatus.FAILED},
    WorkflowStatus.READY: {WorkflowStatus.RUNNING},
    WorkflowStatus.RUNNING: {WorkflowStatus.PAUSED, WorkflowStatus.CANCELLING, WorkflowStatus.COMPLETED, WorkflowStatus.FAILED, WorkflowStatus.PARTIALLY_COMPLETED},
    WorkflowStatus.PAUSED: {WorkflowStatus.RUNNING},
    WorkflowStatus.CANCELLING: {WorkflowStatus.CANCELLED},
}


class InvalidStateTransition(RuntimeError):
    pass


class StateManager:
    def __init__(self, workflow_status: WorkflowStatus = WorkflowStatus.CREATED):
        self.workflow_status = workflow_status
        self.history: list[dict] = []

    def transition(self, status: WorkflowStatus) -> None:
        allowed = _ALLOWED.get(self.workflow_status, set())
        if status != self.workflow_status and status not in allowed:
            raise InvalidStateTransition(f"cannot transition workflow from {self.workflow_status.value} to {status.value}")
        self.workflow_status = status
        self.history.append({"status": status.value, "timestamp": _now().isoformat()})


@dataclass
class StepState:
    status: StepStatus = StepStatus.PENDING
    started_at: datetime | None = None
    completed_at: datetime | None = None
    attempts: int = 0
    history: list[dict] = field(default_factory=list)
    error: str | None = None

    def transition(self, status: StepStatus) -> None:
        allowed = {
            StepStatus.PENDING: {StepStatus.READY, StepStatus.SKIPPED},
            StepStatus.READY: {StepStatus.RUNNING, StepStatus.SKIPPED, StepStatus.CANCELLED},
            StepStatus.RUNNING: {StepStatus.COMPLETED, StepStatus.FAILED, StepStatus.RETRYING, StepStatus.CANCELLED},
            StepStatus.RETRYING: {StepStatus.RUNNING, StepStatus.READY, StepStatus.FAILED},
            StepStatus.FAILED: set(),
            StepStatus.COMPLETED: set(),
            StepStatus.SKIPPED: set(),
            StepStatus.CANCELLED: set(),
        }
        if status != self.status and status not in allowed.get(self.status, set()):
            raise InvalidStateTransition(f"cannot transition step from {self.status.value} to {status.value}")
        now = _now()
        if status == StepStatus.RUNNING and self.status != StepStatus.RETRYING:
            self.started_at = now
        if status == StepStatus.RUNNING and self.started_at is None:
            self.started_at = now
        if status in {StepStatus.COMPLETED, StepStatus.FAILED, StepStatus.SKIPPED, StepStatus.CANCELLED}:
            self.completed_at = now
        self.status = status
        self.history.append({"status": status.value, "timestamp": now.isoformat()})
