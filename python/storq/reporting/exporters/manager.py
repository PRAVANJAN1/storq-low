from pathlib import Path

from .json_exporter import JSONExporter
from .csv_exporter import CSVExporter
from .html_exporter import HTMLExporter
from .pdf_exporter import PDFExporter


class ExportManager:
    def __init__(self):
        self._exporters = {
            "json": JSONExporter(), "csv": CSVExporter(),
            "html": HTMLExporter(), "htm": HTMLExporter(),
            "pdf": PDFExporter(),
        }

    def export(self, report, path: Path, format: str):
        format = format.lower().lstrip(".")
        if format not in self._exporters:
            raise ValueError(f"unsupported report format: {format}")
        return self._exporters[format].export(report, path)
