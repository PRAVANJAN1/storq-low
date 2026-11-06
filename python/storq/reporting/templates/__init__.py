from ..registry import registry

TEMPLATES = (
    "database_inspection", "page_analysis", "wal_analysis", "fault_injection",
    "crash_simulation", "recovery_verification", "forensics",
    "workload_execution", "comprehensive",
)

for template in TEMPLATES:
    registry.register(template, template)
