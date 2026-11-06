from collections.abc import Mapping
from copy import copy
from threading import Lock


class WorkflowContext:
    def __init__(self, workflow_id: str, config: Mapping | None = None, runtime=None, adapter=None,
                 cancellation=None, resource_limits: Mapping | None = None):
        self.workflow_id = workflow_id
        self.config = dict(config or {})
        self.runtime = runtime
        self.database_adapter = adapter
        self.cancellation = cancellation
        self.resource_limits = dict(resource_limits or {})
        self._outputs: dict[str, object] = {}
        self.metadata: dict[str, object] = {}
        self.history: list[dict] = []
        self._lock = Lock()

    def set_output(self, step_name: str, value: object) -> None:
        with self._lock:
            self._outputs[step_name] = value

    def output(self, step_name: str, default=None):
        with self._lock:
            if step_name not in self._outputs:
                if default is not None:
                    return default
                raise KeyError(f"unknown step output: {step_name}")
            return self._outputs[step_name]

    @property
    def outputs(self) -> dict[str, object]:
        with self._lock:
            return copy(self._outputs)

    @property
    def database_path(self):
        return self.config.get("database_path", self.config.get("database"))
