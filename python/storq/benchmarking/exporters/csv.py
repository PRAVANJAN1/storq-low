import csv
from pathlib import Path


def export_csv(result, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["iteration", "elapsed_seconds", "operation_count", "successful_operations", "failed_operations"])
        for index, measurement in enumerate(result.measurements):
            writer.writerow([
                index, measurement.elapsed_seconds, measurement.operation_count,
                measurement.successful_operations, measurement.failed_operations,
            ])
