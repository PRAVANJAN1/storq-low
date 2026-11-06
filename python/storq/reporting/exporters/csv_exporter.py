import csv
from pathlib import Path
from typing import Iterable

from .base import ReportExporter
from ..models import Report, ReportExportResult


class CSVExporter(ReportExporter):
    def export(self, report: Report, path: Path) -> ReportExportResult:
        self.validate_path(path)
        rows = []
        columns = {"section", "title", "property", "value"}
        for section in report.sections:
            for key, value in section.content.items():
                rows.append({"section": section.section_id, "title": section.title,
                             "property": key, "value": value})
        for section in report.sections:
            for table in section.tables:
                for table_row in table.rows:
                    columns.update(table_row.keys())
                    rows.append({"section": section.section_id, "title": table.title, **table_row})
        with path.open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=sorted(columns | set().union(*[row.keys() for row in rows]) if rows else columns))
            writer.writeheader()
            writer.writerows(rows)
        return ReportExportResult(str(path), "csv", path.stat().st_size)
