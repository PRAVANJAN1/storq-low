import json
from pathlib import Path
from typing import Any

from .schema import SCHEMA_VERSION, REQUIRED_WORKFLOW_FIELDS
from ..steps import registration


class SerializationError(ValueError):
    pass


def serialize(value: Any) -> dict[str, Any]:
    if hasattr(value, "to_dict"):
        value = value.to_dict()
    if not isinstance(value, dict):
        raise SerializationError("only workflow definitions and JSON-compatible mappings can be serialized")
    return _json_safe(value)


def serialize_json(value: Any) -> str:
    return json.dumps(serialize(value), sort_keys=True, separators=(",", ":"))


def save(value: Any, path: str) -> None:
    Path(path).write_text(serialize_json(value), encoding="utf-8")


def deserialize(data: Any):
    if isinstance(data, str):
        try:
            data = json.loads(data)
        except json.JSONDecodeError as error:
            raise SerializationError("invalid workflow JSON") from error
    elif isinstance(data, Path):
        data = json.loads(data.read_text(encoding="utf-8"))
    if isinstance(data, dict) and "workflow_definition" in data:
        data = data["workflow_definition"]
    if not isinstance(data, dict):
        raise SerializationError("workflow data must be a JSON object")
    if data.get("schema_version") != SCHEMA_VERSION:
        raise SerializationError("unsupported workflow schema version")
    missing = REQUIRED_WORKFLOW_FIELDS - data.keys()
    if missing:
        raise SerializationError(f"workflow is missing required fields: {sorted(missing)}")

    from ..workflow import Workflow
    from ..config import WorkflowConfig
    from ..steps import WorkflowStep
    from ..steps.registry import registry
    from ..builder import WorkflowBuilder

    config = WorkflowConfig(**{
        key: value for key, value in data.get("configuration", {}).items()
        if key in WorkflowConfig.__dataclass_fields__
    })
    builder = WorkflowBuilder(data["name"], description=data.get("description", ""), config=config)
    builder.workflow.id = data.get("workflow_id", builder.workflow.id)
    for item in data["steps"]:
        cls = registry.get(item["type"].lower()) if item["type"].lower() in _registry_names() else None
        if cls is None:
            raise SerializationError(f"unknown workflow step type {item['type']!r}")
        retry_data = item.get("retry", {})
        kwargs = {
            "config": item.get("configuration", {}),
            "dependencies": item.get("dependencies", []),
            "timeout": item.get("timeout"),
            "idempotent": item.get("idempotent", True),
        }
        step = cls(item["name"], **kwargs)
        builder.workflow.add(step)
    builder.workflow.validate()
    return builder.workflow


def _registry_names():
    from ..steps.registry import registry
    return set(registry._steps)


def _json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    raise SerializationError(f"value of type {type(value).__name__} cannot be serialized safely")
