from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
import uuid


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


@dataclass
class ReportMetadata:
    report_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    report_type: str = "comprehensive"
    title: str = "Storq Report"
    generated_at: datetime = field(default_factory=utcnow)
    storq_version: str = "0.1.0"
    database_engine: str | None = None
    database_format: str | None = None


@dataclass
class ReportEvidence:
    evidence_id: str
    source: str
    evidence_type: str
    reference: str | None = None
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class ReportFinding:
    finding_id: str
    title: str
    severity: str = "info"
    category: str = "observation"
    status: str = "confirmed"
    description: str = ""
    evidence_ids: list[str] = field(default_factory=list)
    source_module: str | None = None


@dataclass
class ReportStatistics:
    values: dict[str, Any] = field(default_factory=dict)


@dataclass
class ReportChart:
    chart_type: str
    title: str
    data: dict[str, Any] = field(default_factory=dict)
    labels: list[str] = field(default_factory=list)


@dataclass
class ReportTimeline:
    title: str
    events: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class ReportTable:
    title: str
    columns: list[str] = field(default_factory=list)
    rows: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class ReportSection:
    section_id: str
    title: str
    content: dict[str, Any] = field(default_factory=dict)
    tables: list[ReportTable] = field(default_factory=list)
    charts: list[ReportChart] = field(default_factory=list)
    timeline: ReportTimeline | None = None


@dataclass
class ReportExportResult:
    path: str
    format: str
    bytes_written: int


@dataclass
class Report:
    metadata: ReportMetadata
    description: str = ""
    sections: list[ReportSection] = field(default_factory=list)
    findings: list[ReportFinding] = field(default_factory=list)
    statistics: ReportStatistics = field(default_factory=ReportStatistics)
    evidence: list[ReportEvidence] = field(default_factory=list)
    source_modules: list[str] = field(default_factory=list)
    limitations: list[str] = field(default_factory=list)
    source_results: list[Any] = field(default_factory=list)
    export_metadata: dict[str, Any] = field(default_factory=dict)

    def section(self, section_id: str) -> ReportSection:
        for item in self.sections:
            if item.section_id == section_id:
                return item
        raise KeyError(section_id)

    def visualize(self, chart_type: str, output: str | Path | None = None, interactive: bool = False):
        from .visualization.engine import VisualizationEngine
        return VisualizationEngine().render(self, chart_type, output=output, interactive=interactive)

    def export(self, path: str | Path, format: str = "json") -> ReportExportResult:
        from .exporters.manager import ExportManager
        return ExportManager().export(self, Path(path), format)

    def to_dict(self) -> dict[str, Any]:
        from .serialization import report_to_dict
        return report_to_dict(self)

    def save(self, path: str | Path, format: str = "json") -> ReportExportResult:
        return self.export(path, format)

    def add_finding(self, finding: ReportFinding):
        self.findings.append(finding)
        return self
