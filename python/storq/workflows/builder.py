from .workflow import Workflow
from .config import WorkflowConfig
from .steps.registry import registry
from .steps.native import (
    FormatInspectionStep, DatabaseInspectionStep, PageAnalysisStep, WALAnalysisStep,
    SnapshotCreationStep, FaultInjectionStep, CrashSimulationStep,
    RecoveryVerificationStep, ForensicInvestigationStep, WorkloadExecutionStep,
    WorkloadReplayStep,
)
from .steps.base import RetryPolicy


class WorkflowBuilder:
    def __init__(self, name: str, description: str = "", config=None):
        self.workflow = Workflow(name=name, description=description, config=config or {})
        self._names = set()

    def add_step(self, step, name=None, depends_on=None):
        if name is not None and name != step.name:
            step.name = step.id = name
        if depends_on:
            step.dependencies = list(depends_on)
        unknown = set(step.dependencies) - self._names
        if unknown:
            raise ValueError(f"unknown dependencies: {sorted(unknown)}")
        if step.name in self._names:
            raise ValueError(f"duplicate step name: {step.name}")
        self.workflow.add(step)
        self._names.add(step.name)
        return self

    def _add(self, cls, default_name: str, config=None, name=None, depends_on=None, **kwargs):
        step_name = name or f"{default_name}_{len(self.workflow.steps) + 1}"
        unknown = set(depends_on or []) - self._names
        if unknown:
            raise ValueError(f"unknown dependencies: {sorted(unknown)}")
        step = cls(step_name, config=config or {}, dependencies=list(depends_on or []), **kwargs)
        return self.add_step(step, depends_on=None)

    def inspect_format(self, database_path, name=None, depends_on=None, **kwargs):
        return self._add(FormatInspectionStep, "format", {"database_path": database_path},
                         name=name, depends_on=depends_on, **kwargs)

    def inspect_database(self, database_path=None, name=None, depends_on=None, **kwargs):
        return self._add(DatabaseInspectionStep, "database", {"database_path": database_path},
                         name=name, depends_on=depends_on, **kwargs)

    def analyze_pages(self, database_path, name=None, depends_on=None, **kwargs):
        return self._add(PageAnalysisStep, "pages", {"database_path": database_path},
                         name=name, depends_on=depends_on, **kwargs)

    def analyze_wal(self, database_path, name=None, depends_on=None, **kwargs):
        return self._add(WALAnalysisStep, "wal", {"database_path": database_path},
                         name=name, depends_on=depends_on, **kwargs)

    def create_snapshot(self, database_path=None, name=None, depends_on=None, **kwargs):
        return self._add(SnapshotCreationStep, "snapshot", {"database_path": database_path},
                         name=name, depends_on=depends_on, **kwargs)

    def inject_fault(self, fault_type=None, seed=None, name=None, depends_on=None, **kwargs):
        config = {"fault_type": fault_type, "seed": seed}
        return self._add(FaultInjectionStep, "fault", config, name=name,
                         depends_on=depends_on, idempotent=False, **kwargs)

    def simulate_crash(self, trigger=None, value=None, name=None, depends_on=None, **kwargs):
        config = {"trigger": trigger, "value": value}
        return self._add(CrashSimulationStep, "crash", config, name=name,
                         depends_on=depends_on, idempotent=False, **kwargs)

    def verify_recovery(self, name=None, depends_on=None, **kwargs):
        return self._add(RecoveryVerificationStep, "recovery", {}, name=name, depends_on=depends_on, **kwargs)

    def investigate(self, name=None, depends_on=None, **kwargs):
        return self._add(ForensicInvestigationStep, "forensics", {}, name=name, depends_on=depends_on, **kwargs)

    def execute_workload(self, workload=None, name=None, depends_on=None, **kwargs):
        return self._add(WorkloadExecutionStep, "workload", {"workload": workload or {}},
                         name=name, depends_on=depends_on, **kwargs)

    def replay_workload(self, workload=None, name=None, depends_on=None, **kwargs):
        return self._add(WorkloadReplayStep, "replay", {"workload": workload or {}},
                         name=name, depends_on=depends_on, **kwargs)

    def run_test_suite(self, suite=None, name=None, depends_on=None, **kwargs):
        from .steps.base import WorkflowStep

        class TestSuiteStep(WorkflowStep):
            async def execute(self, context):
                if hasattr(suite, "run"):
                    return await suite.run(context=context)
                raise ValueError("test suite must implement an async run() method")

        return self._add(
            TestSuiteStep, "test_suite", {"suite": suite},
            name=name, depends_on=depends_on, **kwargs)

    def run_benchmark(self, profile="mixed", iterations=10, name=None, depends_on=None, **kwargs):
        from .steps.base import WorkflowStep

        class BenchmarkStep(WorkflowStep):
            async def execute(self, context):
                operation = kwargs.get("operation")
                if operation is None:
                    raise ValueError("workflow benchmark steps require an explicit operation callable")
                from storq import Benchmark
                benchmark = Benchmark(
                    name=name or "workflow benchmark",
                    database=kwargs.get("database"),
                    profile=profile,
                )
                return await benchmark.run(iterations=iterations, warmup=kwargs.get("warmup", 0),
                                           operation=operation)

        return self._add(
            BenchmarkStep, "benchmark", {"profile": profile, "iterations": iterations},
            name=name, depends_on=depends_on, **kwargs)

    def configure(self, **config):
        for key, value in config.items():
            if hasattr(self.workflow.config, key):
                setattr(self.workflow.config, key, value)
            else:
                self.workflow.context.config[key] = value
        return self

    def build(self):
        self.workflow.validate()
        return self.workflow
