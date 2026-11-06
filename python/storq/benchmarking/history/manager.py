import json
from datetime import datetime
from pathlib import Path


def benchmark_result_to_dict(result) -> dict:
    return {
        "schema_version": 1,
        "benchmark_id": result.benchmark_id,
        "name": result.name,
        "status": result.status,
        "profile": result.profile,
        "database": result.database,
        "seed": result.seed,
        "environment": result.environment,
        "configuration": result.configuration,
        "aggregate": result.aggregate,
        "measurements": [
            {
                "elapsed_seconds": item.elapsed_seconds,
                "operation_count": item.operation_count,
                "successful_operations": item.successful_operations,
                "failed_operations": item.failed_operations,
                "operation_latencies_ns": item.operation_latencies_ns,
            }
            for item in result.measurements
        ],
        "errors": result.errors,
    }


class BenchmarkHistory:
    def __init__(self, directory: str | Path | None = None):
        self.directory = Path(directory) if directory else None
        self._items: dict[str, dict] = {}
        if self.directory and self.directory.exists():
            for path in self.directory.glob("*.json"):
                try:
                    data = json.loads(path.read_text(encoding="utf-8"))
                    self._items[data["benchmark_id"]] = data
                except (OSError, json.JSONDecodeError, KeyError):
                    continue

    def add(self, result) -> None:
        data = benchmark_result_to_dict(result)
        self._items[result.benchmark_id] = data
        if self.directory:
            self.directory.mkdir(parents=True, exist_ok=True)
            (self.directory / f"{result.benchmark_id}.json").write_text(
                json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

    def get(self, benchmark_id: str):
        return self._items.get(benchmark_id)

    def list(self, profile=None, engine=None, status=None, start=None, end=None, limit=20):
        output = []
        for data in self._items.values():
            if profile and data.get("profile") != profile:
                continue
            if engine and data.get("environment", {}).get("database_engine") != engine:
                continue
            if status and data.get("status") != status:
                continue
            output.append(data)
        return output[:max(0, limit)]
