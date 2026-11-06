import json
from pathlib import Path

from ..history.manager import benchmark_result_to_dict


def export_json(result, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(benchmark_result_to_dict(result), indent=2), encoding="utf-8")
