from datetime import datetime, timezone
from pathlib import Path
from typing import Any
import enum

from .models import ReportEvidence, ReportFinding, ReportSection, ReportStatistics, ReportTable


def _value(obj: Any, name: str, default=None):
    if isinstance(obj, dict):
        return obj.get(name, default)
    return getattr(obj, name, default)


def _scalar(value: Any):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, enum.Enum):
        return value.value
    if isinstance(value, datetime):
        return value.isoformat()
    if hasattr(value, "to_dict") and callable(value.to_dict):
        return value.to_dict()
    return str(value)


def normalize_result(result: Any, source_module: str = "unknown") -> dict[str, Any]:
    if isinstance(result, dict):
        data = dict(result)
    elif hasattr(result, "to_dict") and callable(result.to_dict):
        data = result.to_dict()
        data["_source_object"] = result
    else:
        data = {}
        for name in dir(result):
            if name.startswith("_") or callable(getattr(result, name, None)):
                continue
            try:
                data[name] = _scalar(getattr(result, name))
            except (AttributeError, RuntimeError):
                pass
    return {"source_module": source_module, "data": data, "source_result": result}


class ReportDataNormalizer:
    SOURCE_NAMES = {
        "FormatInspectionResult": "format_inspection",
        "WorkflowResult": "workflow",
        "BenchmarkResult": "benchmark",
        "PageAnalysisResult": "page_analysis",
        "WALAnalysisResult": "wal_analysis",
    }

    def normalize(self, result: Any, source_module: str | None = None) -> dict[str, Any]:
        inferred = source_module or self.SOURCE_NAMES.get(type(result).__name__, "module_result")
        normalized = normalize_result(result, inferred)
        if inferred == "format_inspection":
            normalized["report_section"] = self._format_section(result)
        return normalized

    def _format_section(self, result: Any) -> ReportSection:
        format_name = _value(result, "format", "unknown")
        values = {
            "database_format": _scalar(format_name),
            "file_size": _scalar(_value(result, "file_size")),
            "page_size": _scalar(_value(result, "page_size")),
            "page_count": _scalar(_value(result, "page_count")),
            "header_valid": _scalar(_value(result, "header_valid")),
            "structure_valid": _scalar(_value(result, "structure_valid")),
        }
        findings = []
        evidence_ids = []
        if values["header_valid"] is False:
            evidence_ids = ["format-header"]
            findings.append(ReportFinding(
                finding_id="format-header-invalid", title="Database header validation failed",
                severity="high", category="format", description="The native format inspector reported an invalid header.",
                evidence_ids=evidence_ids, source_module="format_inspection"))
        return ReportSection("database_format", "Database Overview", values, [
            ReportTable("Format inspection", ["property", "value"],
                        [{"property": key, "value": value} for key, value in values.items()])
        ])

    def finding_from_result(self, result: Any, index: int = 0) -> ReportFinding | None:
        title = _value(result, "title", _value(result, "message"))
        if title is None:
            return None
        return ReportFinding(
            finding_id=f"finding-{index}",
            title=str(title),
            severity=str(_value(result, "severity", "info")),
            category=str(_value(result, "category", "observation")),
            status=str(_value(result, "status", "confirmed")),
            description=str(_value(result, "description", "")),
            source_module=_value(result, "source_module"),
        )
