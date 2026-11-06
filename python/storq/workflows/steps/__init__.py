from .base import WorkflowStep, RetryPolicy
from .registry import StepRegistry, registry
from .native import (
    FormatInspectionStep, DatabaseInspectionStep, PageAnalysisStep, WALAnalysisStep,
    SnapshotCreationStep, FaultInjectionStep, CrashSimulationStep,
    RecoveryVerificationStep, ForensicInvestigationStep, WorkloadExecutionStep,
    WorkloadReplayStep,
)
from . import registration


def inspect_format(database_path, name="format", depends_on=None, **kwargs):
    return FormatInspectionStep(name, config={"database_path": database_path},
                                dependencies=list(depends_on or []), **kwargs)

def analyze_pages(database_path, name="pages", depends_on=None, **kwargs):
    return PageAnalysisStep(name, config={"database_path": database_path},
                            dependencies=list(depends_on or []), **kwargs)

def analyze_wal(database_path, name="wal", depends_on=None, **kwargs):
    return WALAnalysisStep(name, config={"database_path": database_path},
                           dependencies=list(depends_on or []), **kwargs)

def create_snapshot(database_path, name="snapshot", depends_on=None, **kwargs):
    return SnapshotCreationStep(name, config={"database_path": database_path},
                                dependencies=list(depends_on or []), **kwargs)

def inject_fault(fault_type=None, seed=None, name="fault", depends_on=None, **kwargs):
    return FaultInjectionStep(name, config={"fault_type": fault_type, "seed": seed},
                              dependencies=list(depends_on or []), idempotent=False, **kwargs)

def simulate_crash(trigger=None, value=None, name="crash", depends_on=None, **kwargs):
    return CrashSimulationStep(name, config={"trigger": trigger, "value": value},
                               dependencies=list(depends_on or []), idempotent=False, **kwargs)

def verify_recovery(name="recovery", depends_on=None, **kwargs):
    return RecoveryVerificationStep(name, dependencies=list(depends_on or []), **kwargs)

def investigate(name="forensics", depends_on=None, **kwargs):
    return ForensicInvestigationStep(name, dependencies=list(depends_on or []), **kwargs)

__all__ = [
    "WorkflowStep", "RetryPolicy", "StepRegistry", "registry",
    "FormatInspectionStep", "DatabaseInspectionStep", "PageAnalysisStep",
    "WALAnalysisStep", "SnapshotCreationStep", "FaultInjectionStep",
    "CrashSimulationStep", "RecoveryVerificationStep",
    "ForensicInvestigationStep", "WorkloadExecutionStep", "WorkloadReplayStep",
]
