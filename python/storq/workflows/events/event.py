from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any


@dataclass
class WorkflowEvent:
    type: str
    workflow_id: str
    step_name: str | None = None
    timestamp: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    duration: float | None = None
    metadata: dict[str, Any] = field(default_factory=dict)
