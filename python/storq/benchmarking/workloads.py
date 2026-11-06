from dataclasses import dataclass, field
from typing import Callable, Any


@dataclass
class BenchmarkWorkload:
    name: str
    operation: Callable[[], Any]
    category: str = "mixed"
    configuration: dict[str, Any] = field(default_factory=dict)


def workload(name: str, category: str = "mixed"):
    def decorator(operation):
        operation.__storq_benchmark__ = BenchmarkWorkload(name, operation, category)
        return operation
    return decorator


class WorkloadManager:
    def __init__(self):
        self._workloads: dict[str, BenchmarkWorkload] = {}

    def register(self, workload_item: BenchmarkWorkload) -> None:
        self._workloads[workload_item.name] = workload_item

    def get(self, name: str) -> BenchmarkWorkload:
        if name not in self._workloads:
            raise KeyError(f"unknown benchmark workload: {name}")
        return self._workloads[name]


def create_workload(name: str, operation: Callable[[], Any], category: str = "mixed",
                    **configuration) -> BenchmarkWorkload:
    if operation is None or not callable(operation):
        raise ValueError("benchmark workloads require an explicit operation callable")
    return BenchmarkWorkload(name=name, operation=operation, category=category,
                             configuration=dict(configuration))
