from .base import WorkflowStep


class StepRegistry:
    def __init__(self):
        self._steps: dict[str, type[WorkflowStep]] = {}

    def register(self, name: str, step_type=None):
        if step_type is not None:
            self._steps[name] = step_type
            return step_type

        def decorator(cls):
            self._steps[name] = cls
            return cls
        return decorator

    def unregister(self, name: str) -> None:
        self._steps.pop(name, None)

    def create(self, name: str, step_name: str, **kwargs) -> WorkflowStep:
        if name not in self._steps:
            raise KeyError(f"workflow step type {name!r} is not registered")
        return self._steps[name](step_name, **kwargs)

    def get(self, name: str) -> type[WorkflowStep]:
        return self._steps[name]


registry = StepRegistry()
