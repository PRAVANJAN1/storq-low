import json
from datetime import datetime
from pathlib import Path
from typing import Iterable

from .result import WorkflowResult


class WorkflowHistory:
    def __init__(self, path: str | None = None):
        self.path = Path(path) if path else None
        self._items: list[dict] = []
        if self.path and self.path.exists():
            for line in self.path.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    self._items.append(json.loads(line))

    def record(self, result: WorkflowResult, template: str | None = None) -> None:
        item = {
            "workflow_id": result.workflow_id,
            "status": result.status,
            "start_time": result.start_time.isoformat(),
            "completion_time": result.completion_time.isoformat(),
            "template": template,
            "steps": {
                name: {"status": step.status, "attempts": step.attempts,
                       "error": str(step.error) if step.error else None}
                for name, step in result.step_results.items()
            },
        }
        self._items.append(item)
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a", encoding="utf-8") as file:
                file.write(json.dumps(item, sort_keys=True) + "\n")

    def by_id(self, workflow_id: str) -> list[dict]:
        return [item for item in self._items if item["workflow_id"] == workflow_id]

    def by_status(self, status: str) -> list[dict]:
        return [item for item in self._items if item["status"] == status]

    def by_template(self, template: str) -> list[dict]:
        return [item for item in self._items if item.get("template") == template]

    def between(self, start: datetime, end: datetime) -> list[dict]:
        return [
            item for item in self._items
            if start <= datetime.fromisoformat(item["start_time"]) <= end
        ]
