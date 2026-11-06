from .registry import registry
from .native import (
    FormatInspectionStep, DatabaseInspectionStep, PageAnalysisStep, WALAnalysisStep,
    SnapshotCreationStep, FaultInjectionStep, CrashSimulationStep,
    RecoveryVerificationStep, ForensicInvestigationStep, WorkloadExecutionStep,
    WorkloadReplayStep,
)

for step_type in (
    FormatInspectionStep, DatabaseInspectionStep, PageAnalysisStep, WALAnalysisStep,
    SnapshotCreationStep, FaultInjectionStep, CrashSimulationStep,
    RecoveryVerificationStep, ForensicInvestigationStep, WorkloadExecutionStep,
    WorkloadReplayStep,
):
    registry.register(step_type.__name__.lower(), step_type)
