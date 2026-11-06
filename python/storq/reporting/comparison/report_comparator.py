from .comparison_result import ReportComparison


def compare_reports(previous, current) -> ReportComparison:
    previous_type = previous.metadata.report_type
    current_type = current.metadata.report_type
    if previous_type != current_type:
        return ReportComparison(
            previous.metadata.report_id, current.metadata.report_id, False,
            reason=f"incompatible report types: {previous_type} and {current_type}")
    previous_findings = {item.title: item for item in previous.findings}
    current_findings = {item.title: item for item in current.findings}
    previous_keys = set(previous_findings)
    current_keys = set(current_findings)
    severity_changes = {}
    for title in previous_keys & current_keys:
        old = previous_findings[title].severity
        new = current_findings[title].severity
        if old != new:
            severity_changes[title] = {"previous": old, "current": new}
    metadata_changes = {}
    old_meta = previous.metadata
    new_meta = current.metadata
    for key in ("database_engine", "database_format", "storq_version"):
        old_value, new_value = getattr(old_meta, key), getattr(new_meta, key)
        if old_value != new_value:
            metadata_changes[key] = (old_value, new_value)
    statistic_changes = {}
    for key in set(previous.statistics.values) | set(current.statistics.values):
        old_value = previous.statistics.values.get(key)
        new_value = current.statistics.values.get(key)
        if old_value != new_value:
            statistic_changes[key] = (old_value, new_value)
    return ReportComparison(
        previous.metadata.report_id, current.metadata.report_id,
        new_findings=sorted(current_keys - previous_keys),
        resolved_findings=sorted(previous_keys - current_keys),
        persistent_findings=sorted(previous_keys & current_keys),
        severity_changes=severity_changes,
        metadata_changes=metadata_changes,
        statistic_changes=statistic_changes,
    )
