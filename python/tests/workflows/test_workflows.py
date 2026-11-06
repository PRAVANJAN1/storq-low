import asyncio
import json

import storq
from storq.workflows.steps.base import WorkflowStep


class ValueStep(WorkflowStep):
    async def execute(self, context):
        return self.config["value"]


def test_workflow_builder_and_execution():
    workflow = (
        storq.WorkflowBuilder("test")
        .add_step(ValueStep("one", config={"value": 1}))
        .add_step(ValueStep("two", config={"value": 2}, dependencies=["one"]))
        .build()
    )
    result = asyncio.run(workflow.execute())
    assert result.status == "completed"
    assert result.output("two") == 2
    assert result.completed_steps == ["one", "two"]


def test_dependency_failure_skips_dependants():
    class FailingStep(WorkflowStep):
        async def execute(self, context):
            raise RuntimeError("expected failure")

    workflow = storq.Workflow("failure")
    workflow.add(FailingStep("first"))
    workflow.add(ValueStep("second", dependencies=["first"], config={"value": 2}))
    result = asyncio.run(workflow.execute())
    assert result.status == "failed"
    assert result.skipped_steps == ["second"]


def test_cycle_is_rejected():
    workflow = storq.Workflow("cycle")
    workflow.add(ValueStep("one", dependencies=["two"], config={"value": 1}))
    workflow.add(ValueStep("two", dependencies=["one"], config={"value": 2}))
    try:
        workflow.validate()
        assert False, "cycle should have been rejected"
    except ValueError:
        pass


def test_serialization_round_trip():
    workflow = storq.WorkflowBuilder("serialized").inspect_format("test.db", name="format").build()
    data = storq.workflows.serialize(workflow)
    assert json.loads(storq.workflows.serialize_json(workflow))
    restored = storq.workflows.deserialize(data)
    assert restored.name == workflow.name
