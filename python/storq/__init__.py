try:
    from . import _native as _native
except ImportError:
    import types
    import sys
    _native = types.ModuleType("_native")
    _native.__getattr__ = lambda name: None
    sys.modules[__name__ + "._native"] = _native

Runtime = getattr(_native, "Runtime", None)
RuntimeState = getattr(_native, "RuntimeState", None)
SQLiteAdapter = getattr(_native, "SQLiteAdapter", None)
SQLiteInspector = getattr(_native, "SQLiteInspector", None)
DatabaseEngine = getattr(_native, "DatabaseEngine", None)
AdapterState = getattr(_native, "AdapterState", None)
AdapterConfig = getattr(_native, "AdapterConfig", None)
DatabaseConnectionConfig = getattr(_native, "DatabaseConnectionConfig", None)
WorkloadConfig = getattr(_native, "WorkloadConfig", None)
TransactionGenerator = getattr(_native, "TransactionGenerator", None)
TransactionOracle = getattr(_native, "TransactionOracle", None)
KeyDistribution = getattr(_native, "KeyDistribution", None)
SchedulingPolicy = getattr(_native, "SchedulingPolicy", None)
inspect_format = getattr(_native, "inspect_format", None)
supported_storq_page_sizes = getattr(_native, "supported_storq_page_sizes", None)
create_storq_database = getattr(_native, "create_storq_database", None)
inspect_storq_database = getattr(_native, "inspect_storq_database", None)
RecordID = getattr(_native, "RecordID", None)
RecordManager = getattr(_native, "RecordManager", None)
HashIndex = getattr(_native, "HashIndex", None)
NativeWAL = getattr(_native, "NativeWAL", None)

# Workflow orchestration API
from . import workflows
from .workflows import (
    Workflow, WorkflowBuilder, WorkflowConfig, WorkflowContext, WorkflowStatus,
    StepStatus, WorkflowResult, WorkflowEvent, WorkflowHistory,
)
from .workflows import templates
from .workflows import steps
from . import reporting
from .reporting import (
    Report, ReportMetadata, ReportSection, ReportFinding, ReportStatistics,
    ReportEvidence, ReportChart, ReportTimeline, ReportTable, ReportExportResult,
    ReportingConfig, ReportBuilder, ReportingEngine, ReportDataNormalizer,
    ReportComparison, compare_reports, ReportHistory, report_history,
)
from . import testing
from .testing import TestConfig, TestStatus, TestResult, SuiteResult, TestCase, TestSuite
from .testing import (
    page_integrity, wal_integrity, recovery_consistency, storage_reliability,
    fault_tolerance, transaction_consistency, concurrency_reliability,
)
tests = testing
from . import format
from . import benchmarking
from .benchmarking import (
    BenchmarkConfig, BenchmarkMeasurement, BenchmarkResult, BenchmarkEngine, Benchmark,
    BenchmarkSuite, BenchmarkComparison, compare_benchmarks, BenchmarkHistory,
    latency_stats, percentile, BenchmarkWorkload, WorkloadManager, workload, create_workload,
)

__all__ = [
    "Runtime", "RuntimeState", "SQLiteAdapter", "SQLiteInspector",
    "DatabaseEngine", "AdapterState", "AdapterConfig",
    "DatabaseConnectionConfig", "WorkloadConfig", "TransactionGenerator",
    "TransactionOracle", "KeyDistribution", "SchedulingPolicy",
    "inspect_format",
    "supported_storq_page_sizes", "create_storq_database", "inspect_storq_database",
    "RecordID", "RecordManager",
    "HashIndex", "NativeWAL",
    "Workflow", "WorkflowBuilder", "WorkflowConfig", "WorkflowContext",
    "WorkflowStatus", "StepStatus", "WorkflowResult", "WorkflowEvent",
    "WorkflowHistory", "templates", "steps", "workflows",
    "Report", "ReportMetadata", "ReportSection", "ReportFinding",
    "ReportStatistics", "ReportEvidence", "ReportChart", "ReportTimeline",
    "ReportTable", "ReportExportResult", "ReportingConfig", "ReportBuilder",
    "ReportingEngine", "ReportDataNormalizer", "ReportComparison",
    "compare_reports", "ReportHistory", "report_history", "reporting",
    "testing", "TestConfig", "TestStatus", "TestResult", "SuiteResult", "TestCase", "TestSuite",
    "page_integrity", "wal_integrity", "recovery_consistency", "storage_reliability",
    "fault_tolerance", "transaction_consistency", "concurrency_reliability",
    "tests",
    "benchmarking", "BenchmarkConfig", "BenchmarkMeasurement", "BenchmarkResult",
    "BenchmarkEngine", "Benchmark", "BenchmarkSuite", "BenchmarkComparison",
    "compare_benchmarks", "BenchmarkHistory", "latency_stats", "percentile",
    "BenchmarkWorkload", "WorkloadManager", "workload", "create_workload",
]
