from .models import ReportFinding, ReportSection


def summarize_findings(findings: list[ReportFinding]) -> str:
    counts = {}
    for finding in findings:
        counts[finding.severity] = counts.get(finding.severity, 0) + 1
    if not counts:
        return "No findings were established by the available source results."
    critical = counts.get("critical", 0)
    high = counts.get("high", 0)
    return (
        f"The available evidence established {len(findings)} finding(s), "
        f"including {critical} critical and {high} high-severity finding(s)."
    )


def append_summary(report) -> None:
    report.sections.insert(0, ReportSection(
        "executive_summary", "Executive Summary",
        {"summary": summarize_findings(report.findings)}))
