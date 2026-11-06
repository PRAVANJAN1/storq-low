import asyncio
from typing import Any

from ... import _native
from .base import WorkflowStep


class NativeStep(WorkflowStep):
    native_name: str = ""
    destructive: bool = False

    def __init__(self, name: str, *args, **kwargs):
        destructive = kwargs.pop("destructive", self.destructive)
        super().__init__(name, *args, **kwargs)
        if destructive and self.idempotent:
            raise ValueError(f"{self.native_name} is destructive and must be explicitly marked non-idempotent")

    async def call_native(self, function_name: str, *args, **kwargs):
        function = getattr(_native, function_name, None)
        if function is None:
            raise NotImplementedError(
                f"Module 12 does not expose native operation {function_name!r}; "
                "this workflow cannot substitute a fake implementation"
            )
        value = function(*args, **kwargs)
        if callable(getattr(value, "__await__", None)):
            return await value
        return value


class FormatInspectionStep(NativeStep):
    native_name = "format inspection"
    async def execute(self, context):
        return await self.call_native("inspect_format", self.config["database_path"])


class DatabaseInspectionStep(NativeStep):
    native_name = "database inspection"
    async def execute(self, context):
        adapter = context.database_adapter
        if adapter is None:
            raise RuntimeError("database inspection requires a database adapter in the execution context")
        return adapter


def _unsupported(name: str, native_name: str):
    class UnsupportedStep(NativeStep):
        async def execute(self, context):
            raise NotImplementedError(
                f"{native_name} is not exposed by the installed Storq Module 12 SDK"
            )
    UnsupportedStep.__name__ = name
    UnsupportedStep.native_name = native_name
    return UnsupportedStep


PageAnalysisStep = _unsupported("PageAnalysisStep", "page analysis")
WALAnalysisStep = _unsupported("WALAnalysisStep", "WAL analysis")
SnapshotCreationStep = _unsupported("SnapshotCreationStep", "snapshot creation")
FaultInjectionStep = _unsupported("FaultInjectionStep", "fault injection")
CrashSimulationStep = _unsupported("CrashSimulationStep", "crash simulation")
RecoveryVerificationStep = _unsupported("RecoveryVerificationStep", "recovery verification")
ForensicInvestigationStep = _unsupported("ForensicInvestigationStep", "forensic investigation")


class WorkloadExecutionStep(NativeStep):
    native_name = "workload execution"
    async def execute(self, context):
        config = _native.WorkloadConfig()
        values = self.config.get("workload", {})
        for key, value in values.items():
            setattr(config, key, value)
        generator = _native.TransactionGenerator(config)
        return generator.generate()


class WorkloadReplayStep(NativeStep):
    native_name = "workload replay"
    async def execute(self, context):
        raise NotImplementedError("workload replay is not exposed by the installed Storq Module 12 SDK")
