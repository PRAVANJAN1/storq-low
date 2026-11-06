from typing import Any

from .config import ReportingConfig
from .models import Report, ReportMetadata, ReportSection, ReportFinding, ReportEvidence, utcnow
from .normalizer import ReportDataNormalizer, _value, _scalar


class ReportBuilder:
    def __init__(self, title: str = "Storq Report", report_type: str = "comprehensive",
                 description: str = "", config: ReportingConfig | None = None):
        config = config or ReportingConfig()
        config.validate()
        self.report = Report(
            metadata=ReportMetadata(report_type=report_type, title=title),
            description=description,
        )
        self.config = config
        self.normalizer = ReportDataNormalizer()
        self._sources: list[tuple[str, Any]] = []

    def add(self, result: Any, source_module: str = "module_result"):
        normalized = self.normalizer.normalize(result, source_module)
        self._sources.append((source_module, result))
        if "report_section" in normalized:
            self.report.sections.append(normalized["report_section"])
        else:
            section_id = source_module.replace("-", "_")
            self.report.sections.append(ReportSection(
                section_id, source_module.replace("_", " ").title(),
                normalized["data"]))
        if source_module not in self.report.source_modules:
            self.report.source_modules.append(source_module)
        self.report.source_results.append(result)
        finding = self.normalizer.finding_from_result(result)
        if finding:
            finding.source_module = source_module
            self.report.findings.append(finding)
        return self

    add_inspection = lambda self, result: self.add(result, "format_inspection")
    add_page_analysis = lambda self, result: self.add(result, "page_analysis")
    add_wal_analysis = lambda self, result: self.add(result, "wal_analysis")
    add_fault = lambda self, result: self.add(result, "fault_injection")
    add_crash = lambda self, result: self.add(result, "crash_simulation")
    add_recovery = lambda self, result: self.add(result, "recovery_verification")
    add_forensics = lambda self, result: self.add(result, "forensics")
    add_workload = lambda self, result: self.add(result, "workload_execution")
    add_workflow = lambda self, result: self.add(result, "workflow")

    def add_finding(self, finding: ReportFinding):
        self.report.findings.append(finding)
        return self

    def add_evidence(self, evidence: ReportEvidence):
        self.report.evidence.append(evidence)
        return self

    def description(self, text: str):
        self.report.description = text
        return self

    def metadata(self, **values):
        for key, value in values.items():
            if hasattr(self.report.metadata, key):
                setattr(self.report.metadata, key, value)
            else:
                self.report.export_metadata[key] = value
        return self

    def include_sections(self, *section_ids: str):
        allowed = set(section_ids)
        self.report.sections[:] = [item for item in self.report.sections if item.section_id in allowed]
        return self

    def exclude_sections(self, *section_ids: str):
        excluded = set(section_ids)
        self.report.sections[:] = [item for item in self.report.sections if item.section_id not in excluded]
        return self

    def filter_findings(self, severity=None, category=None, status=None):
        self.report.findings[:] = [
            item for item in self.report.findings
            if (severity is None or item.severity in severity)
            and (category is None or item.category in category)
            and (status is None or item.status in status)
        ]
        return self

    def limit_records(self, maximum: int | None = None):
        maximum = self.config.max_displayed_records if maximum is None else maximum
        for section in self.report.sections:
            for table in section.tables:
                table.rows = table.rows[:maximum]
        return self

    def build(self) -> Report:
        if not self.report.source_modules:
            self.report.limitations.append("No source module result was supplied.")
        self.report.export_metadata["schema_version"] = 1
        return self.report
