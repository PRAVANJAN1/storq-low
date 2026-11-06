import json
from datetime import datetime
from pathlib import Path

from ..serialization import report_to_dict


class ReportHistory:
    def __init__(self, directory: str | Path | None = None):
        self.directory = Path(directory) if directory else None
        self._reports = {}
        if self.directory:
            self.directory.mkdir(parents=True, exist_ok=True)
            self._load()

    def _load(self):
        for path in self.directory.glob("*.json"):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                if data.get("schema_version") != 1:
                    continue
                report_id = data["metadata"]["report_id"]
                self._reports[report_id] = data
            except (OSError, json.JSONDecodeError, KeyError, TypeError):
                continue

    def add(self, report) -> None:
        data = report_to_dict(report)
        self._reports[report.metadata.report_id] = data
        if self.directory:
            path = self.directory / f"{report.metadata.report_id}.json"
            path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

    def get(self, report_id: str):
        return self._reports.get(report_id)

    def list(self, report_type: str | None = None, database: str | None = None,
             start: datetime | None = None, end: datetime | None = None, limit: int = 20):
        values = []
        for data in self._reports.values():
            metadata = data["metadata"]
            if report_type and metadata["report_type"] != report_type:
                continue
            if database and metadata.get("database_engine") != database:
                continue
            generated = datetime.fromisoformat(metadata["generated_at"])
            if start and generated < start:
                continue
            if end and generated > end:
                continue
            values.append(data)
        return values[:max(0, limit)]


def report_history(directory: str | Path | None = None) -> ReportHistory:
    return ReportHistory(directory)
