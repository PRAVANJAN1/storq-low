import json
from pathlib import Path

from .base import ReportExporter
from ..models import Report, ReportExportResult
from ..serialization import report_to_dict


class JSONExporter(ReportExporter):
    def export(self, report: Report, path: Path) -> ReportExportResult:
        self.validate_path(path)
        path.write_text(json.dumps(report_to_dict(report), indent=2, ensure_ascii=False), encoding="utf-8")
        return ReportExportResult(str(path), "json", path.stat().st_size)
