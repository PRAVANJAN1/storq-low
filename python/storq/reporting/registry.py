from collections.abc import Callable


class ReportTemplateRegistry:
    def __init__(self):
        self._templates: dict[str, Callable] = {}

    def register(self, name: str, factory=None):
        if factory is not None:
            self._templates[name.lower()] = factory
            return factory

        def decorator(value):
            self._templates[name.lower()] = value
            return value
        return decorator

    def unregister(self, name: str) -> None:
        self._templates.pop(name.lower(), None)

    def get(self, name: str) -> Callable:
        key = name.lower()
        if key not in self._templates:
            raise KeyError(f"unknown report template: {name}")
        return self._templates[key]

    def names(self) -> list[str]:
        return sorted(self._templates)


registry = ReportTemplateRegistry()
