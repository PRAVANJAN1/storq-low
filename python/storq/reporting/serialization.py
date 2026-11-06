from datetime import datetime
import enum
from typing import Any

from .models import Report


def json_value(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, datetime):
        return value.isoformat()
    if isinstance(value, enum.Enum):
        return value.value
    if isinstance(value, dict):
        return {str(key): json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_value(item) for item in value]
    if hasattr(value, "to_dict") and callable(value.to_dict):
        return json_value(value.to_dict())
    return str(value)


def _section_to_dict(section) -> dict:
    return {
        "section_id": section.section_id, "title": section.title,
        "content": json_value(section.content),
        "tables": [{"title": t.title, "columns": t.columns, "rows": json_value(t.rows)} for t in section.tables],
        "charts": [{"chart_type": c.chart_type, "title": c.title, "labels": c.labels,
                    "data": json_value(c.data)} for c in section.charts],
        "timeline": None if section.timeline is None else {
            "title": section.timeline.title, "events": json_value(section.timeline.events)
        },
    }


def report_to_dict(report: Report) -> dict:
    metadata = report.metadata
    return {
        "schema_version": 1,
        "metadata": {
            "report_id": metadata.report_id, "report_type": metadata.report_type,
            "title": metadata.title, "generated_at": metadata.generated_at.isoformat(),
            "storq_version": metadata.storq_version,
            "database_engine": metadata.database_engine,
            "database_format": metadata.database_format,
        },
        "description": report.description,
        "source_modules": report.source_modules,
        "sections": [_section_to_dict(section) for section in report.sections],
        "findings": [json_value({
            "finding_id": f.finding_id, "title": f.title, "severity": f.severity,
            "category": f.category, "status": f.status, "description": f.description,
            "evidence_ids": f.evidence_ids, "source_module": f.source_module,
        }) for f in report.findings],
        "statistics": json_value(report.statistics.values),
        "evidence": [json_value({
            "evidence_id": e.evidence_id, "source": e.source, "evidence_type": e.evidence_type,
            "reference": e.reference, "details": e.details,
        }) for e in report.evidence],
        "limitations": report.limitations,
        "export_metadata": json_value(report.export_metadata),
    }
