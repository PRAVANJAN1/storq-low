from abc import ABC, abstractmethod
from pathlib import Path

from ..models import Report, ReportExportResult


class ReportExporter(ABC):
    @abstractmethod
    def export(self, report: Report, path: Path) -> ReportExportResult:
        raise NotImplementedError

    def validate_path(self, path: Path) -> None:
        if not path or path.is_dir():
            raise ValueError("report output path must identify a file")
        if path.exists() and path.resolve() == Path(__file__).resolve():
            raise ValueError("refusing to overwrite a Storq source file")
        path.parent.mkdir(parents=True, exist_ok=True)
