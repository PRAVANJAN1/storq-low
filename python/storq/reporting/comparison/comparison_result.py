from dataclasses import dataclass, field
from typing import Any


@dataclass
class ReportComparison:
    previous_report_id: str
    current_report_id: str
    comparable: bool = True
    reason: str | None = None
    new_findings: list[str] = field(default_factory=list)
    resolved_findings: list[str] = field(default_factory=list)
    persistent_findings: list[str] = field(default_factory=list)
    severity_changes: dict[str, dict[str, str]] = field(default_factory=dict)
    metadata_changes: dict[str, tuple[Any, Any]] = field(default_factory=dict)
    statistic_changes: dict[str, tuple[Any, Any]] = field(default_factory=dict)
