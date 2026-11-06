import copy
import json
from typing import Any

SCHEMA_VERSION = 1


class CheckpointError(ValueError):
    pass


class CheckpointManager:
    def create(self, workflow) -> dict[str, Any]:
        completed = [
            step.name for step in workflow.steps if step.state.status.value == "completed"
        ]
        outputs = {
            name: value for name, value in workflow.context.outputs.items()
            if self._is_safe(value)
        }
        return {
            "schema_version": SCHEMA_VERSION,
            "workflow_id": workflow.id,
            "workflow_definition": workflow.to_dict(),
            "completed_step_ids": completed,
            "outputs": outputs,
            "execution_state": workflow.state_manager.workflow_status.value,
            "dependency_state": {
                step.name: list(step.dependencies) for step in workflow.steps
            },
            "metadata": copy.deepcopy(workflow.context.metadata),
        }

    def save(self, workflow, path: str) -> dict[str, Any]:
        checkpoint = self.create(workflow)
        with open(path, "w", encoding="utf-8") as file:
            json.dump(checkpoint, file, default=str, sort_keys=True)
        return checkpoint

    def load(self, path_or_dict: str | dict[str, Any]) -> dict[str, Any]:
        if isinstance(path_or_dict, dict):
            checkpoint = copy.deepcopy(path_or_dict)
        else:
            with open(path_or_dict, encoding="utf-8") as file:
                checkpoint = json.load(file)
        if checkpoint.get("schema_version") != SCHEMA_VERSION:
            raise CheckpointError("incompatible checkpoint schema version")
        if not isinstance(checkpoint.get("workflow_definition"), dict):
            raise CheckpointError("checkpoint is missing a workflow definition")
        if "completed_step_ids" not in checkpoint or "dependency_state" not in checkpoint:
            raise CheckpointError("checkpoint is missing execution state")
        return checkpoint

    @staticmethod
    def _is_safe(value: Any) -> bool:
        if value is None or isinstance(value, (str, int, float, bool)):
            return True
        if isinstance(value, (list, dict)):
            try:
                json.dumps(value, default=str)
                return True
            except (TypeError, ValueError):
                return False
        try:
            json.dumps(value, default=str)
            return True
        except (TypeError, ValueError):
            return False
