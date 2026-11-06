from .builder import WorkflowBuilder


def database_health(database=None, database_path=None, **kwargs):
    path = database_path or database
    if not path:
        raise ValueError("database or database_path is required")
    builder = WorkflowBuilder("Database Health Inspection")
    builder.inspect_format(path, name="format")
    builder.inspect_database(path, name="metadata", depends_on=["format"])
    builder.analyze_pages(path, name="pages", depends_on=["metadata"])
    if path.lower().endswith(".db"):
        builder.analyze_wal(path + "-wal", name="wal", depends_on=["pages"])
    return builder.build()


def wal_integrity(database=None, database_path=None, **kwargs):
    path = database_path or database
    if not path:
        raise ValueError("database or database_path is required")
    return (WorkflowBuilder("WAL Integrity Investigation")
            .inspect_format(path, name="format")
            .analyze_wal(path + "-wal", name="wal", depends_on=["format"])
            .investigate(name="forensics", depends_on=["wal"])
            .build())


def crash_recovery(database=None, database_path=None, seed=0, operation_count=100, **kwargs):
    path = database_path or database
    if not path:
        raise ValueError("database or database_path is required")
    return (WorkflowBuilder("Crash Recovery Validation")
            .create_snapshot(path, name="snapshot")
            .execute_workload({"random_seed": seed, "operation_count": operation_count},
                              name="workload", depends_on=["snapshot"])
            .inject_fault(seed=seed, name="fault", depends_on=["workload"])
            .simulate_crash(name="crash", depends_on=["fault"])
            .verify_recovery(name="recovery", depends_on=["crash"])
            .investigate(name="forensics", depends_on=["recovery"])
            .build())


database_health_inspection = database_health
crash_recovery_validation = crash_recovery
wal_integrity_investigation = wal_integrity
