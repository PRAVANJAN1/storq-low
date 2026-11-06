import asyncio
import uuid
from datetime import datetime, timezone

from .config import WorkflowConfig
from .context import WorkflowContext
from .execution.state import StateManager, WorkflowStatus, StepStatus
from .execution.dependency import DependencyResolver
from .execution.executor import WorkflowExecutor
from .execution.checkpoint import CheckpointManager
from .execution.cancellation import CancellationManager
from .events.dispatcher import EventDispatcher
from .results.result import WorkflowResult


def _utcnow():
    return datetime.now(timezone.utc)


class Workflow:
    def __init__(self, name: str, description: str = "", steps=None, config=None, workflow_id=None,
                 runtime=None, adapter=None):
        self.id = workflow_id or str(uuid.uuid4())
        self.name = name
        self.description = description
        self.steps = list(steps or [])
        self.config = config if isinstance(config, WorkflowConfig) else WorkflowConfig(**(config or {}))
        self.config.runtime = self.config.runtime or runtime
        self.config.adapter = self.config.adapter or adapter
        self.state_manager = StateManager()
        self.cancellation = CancellationManager()
        self.events = EventDispatcher()
        self.context = WorkflowContext(
            self.id, self.config.to_dict(), runtime=self.config.runtime, adapter=self.config.adapter,
            cancellation=self.cancellation, resource_limits={"max_concurrency": self.config.max_concurrency})
        self.context.database_adapter = self.config.adapter
        self.history: list[WorkflowResult] = []
        self.step_results = {}
        self._started_at = None
        self._result = None
        self.created_at = _utcnow()
        self.executor = WorkflowExecutor()
        self.checkpoint_manager = CheckpointManager()

    def add(self, step, depends_on=None):
        if depends_on:
            step.dependencies.extend(dep for dep in depends_on if dep not in step.dependencies)
        self.steps.append(step)
        return self

    def validate(self) -> None:
        self.config.validate()
        names = [step.name for step in self.steps]
        if len(names) != len(set(names)):
            raise ValueError("workflow step names must be unique")
        resolver = DependencyResolver(self.steps)
        resolver.validate()
        for step in self.steps:
            step.retry.validate()

    async def execute(self, runtime=None):
        if runtime is not None:
            self.context.runtime = runtime
        if self.state_manager.workflow_status in {WorkflowStatus.COMPLETED, WorkflowStatus.FAILED}:
            raise RuntimeError("workflow cannot be executed twice; create a checkpoint or new workflow")
        operation = self.executor.execute(self)
        self._started_at = _utcnow()
        if self.config.timeout is not None:
            return await asyncio.wait_for(operation, self.config.timeout)
        return await operation

    def run(self):
        try:
            asyncio.get_running_loop()
        except RuntimeError:
            return asyncio.run(self.execute())
        raise RuntimeError("workflow.run() cannot be called from a running event loop; await workflow.execute() instead")

    def cancel(self):
        self.cancellation.request()

    def on(self, event_type: str, handler=None):
        return self.events.subscribe(event_type, handler)

    def off(self, event_type: str, handler):
        return self.events.unsubscribe(event_type, handler)

    def configure(self, **config):
        from .steps.base import RetryPolicy
        for key, value in config.items():
            if key == "retry" and isinstance(value, dict):
                retry = RetryPolicy(
                    max_attempts=value.get("max_attempts", 1),
                    initial_delay=value.get("initial_delay", 0.0),
                    backoff=value.get("backoff", "none"),
                    max_delay=value.get("max_delay"),
                )
                for step in self.steps:
                    step.retry = retry
            elif hasattr(self.config, key):
                setattr(self.config, key, value)
            else:
                self.context.config[key] = value
        return self

    def pause(self):
        self.state_manager.transition(WorkflowStatus.PAUSED)

    def resume(self):
        self.state_manager.transition(WorkflowStatus.RUNNING)

    def checkpoint(self):
        return self.checkpoint_manager.create(self)

    def save_checkpoint(self, path: str):
        return self.checkpoint_manager.save(self, path)

    @classmethod
    def resume(cls, checkpoint, runtime=None, adapter=None):
        manager = CheckpointManager()
        data = manager.load(checkpoint)
        definition = deserialize(data["workflow_definition"])
        workflow = definition._workflow(runtime=runtime, adapter=adapter)
        for name in data["completed_step_ids"]:
            step = next((item for item in workflow.steps if item.name == name), None)
            if step is None:
                raise ValueError(f"checkpoint references missing step {name!r}")
            step.state.status = StepStatus.COMPLETED
            step.state.completed_at = _utcnow()
        for name, output in data.get("outputs", {}).items():
            workflow.context.set_output(name, output)
        workflow.context.metadata.update(data.get("metadata", {}))
        workflow.state_manager.workflow_status = WorkflowStatus.READY
        return workflow

    def to_dict(self):
        return {
            "schema_version": 1,
            "workflow_id": self.id,
            "name": self.name,
            "description": self.description,
            "configuration": self.config.to_dict(),
            "steps": [
                {
                    "name": step.name, "type": step.__class__.__name__,
                    "description": step.description, "configuration": step.config,
                    "dependencies": step.dependencies, "timeout": step.timeout,
                    "retry": step.retry.to_dict(), "idempotent": step.idempotent,
                } for step in self.steps
            ],
        }

    def _workflow(self, runtime=None, adapter=None):
        return self
