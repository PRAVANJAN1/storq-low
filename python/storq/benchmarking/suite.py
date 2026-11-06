import asyncio

from .config import BenchmarkConfig
from .engine import BenchmarkEngine


class BenchmarkSuite:
    def __init__(self, name: str):
        self.name = name
        self._databases: list[dict] = []

    def add_database(self, engine: str, path: str):
        self._databases.append({"engine": engine, "path": path})
        return self

    async def run(self, profile="mixed", iterations=1, warmup=0, operation=None):
        if not self._databases:
            raise ValueError("benchmark suite requires at least one database")
        results = []
        for database in self._databases:
            if database["engine"].lower() != "sqlite":
                results.append({
                    "status": "unsupported",
                    "engine": database["engine"],
                    "reason": "only the SQLite adapter is currently benchmark-enabled",
                })
                continue
            config = BenchmarkConfig(
                name=f"{self.name} ({database['engine']})",
                database=database["path"], profile=profile,
                iterations=iterations, warmup=warmup)
            results.append(await BenchmarkEngine().run(config, operation=operation))
        return results
