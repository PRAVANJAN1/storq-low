class DependencyError(ValueError):
    pass


class DependencyResolver:
    def __init__(self, steps):
        self.steps = {step.name: step for step in steps}

    def validate(self) -> None:
        for step in self.steps.values():
            for dependency in step.dependencies:
                if dependency not in self.steps:
                    raise DependencyError(f"step {step.name!r} depends on unknown step {dependency!r}")
        visiting, visited = set(), set()

        def visit(name):
            if name in visiting:
                raise DependencyError(f"circular dependency involving {name!r}")
            if name in visited:
                return
            visiting.add(name)
            for dependency in self.steps[name].dependencies:
                visit(dependency)
            visiting.remove(name)
            visited.add(name)

        for name in self.steps:
            visit(name)

    def ready(self, completed: set[str], running: set[str]) -> list[str]:
        ready = []
        for name, step in self.steps.items():
            if name in completed or name in running:
                continue
            if all(dependency in completed for dependency in step.dependencies):
                ready.append(name)
        return ready
