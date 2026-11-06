import json
from pathlib import Path

import storq


class Inspection:
    format = "SQLite"
    file_size = 4096
    page_size = 4096
    page_count = 1
    header_valid = True
    structure_valid = True


def test_builder_and_exports(tmp_path):
    report = (
        storq.ReportBuilder("Test inspection", report_type="database_inspection")
        .add_inspection(Inspection())
        .build()
    )
    json_path = tmp_path / "report.json"
    report.export(json_path, "json")
    data = json.loads(json_path.read_text(encoding="utf-8"))
    assert data["schema_version"] == 1
    assert data["metadata"]["title"] == "Test inspection"

    html_path = tmp_path / "report.html"
    report.export(html_path, "html")
    html = html_path.read_text(encoding="utf-8")
    assert "<html" in html

    csv_path = tmp_path / "report.csv"
    report.export(csv_path, "csv")
    assert csv_path.stat().st_size > 0


def test_html_escapes_database_text(tmp_path):
    result = type("Inspection", (), {
        "format": "<script>alert(1)</script>", "file_size": 1,
        "page_size": 4096, "page_count": 1, "header_valid": True,
        "structure_valid": True,
    })()
    report = storq.ReportBuilder().add_inspection(result).build()
    path = tmp_path / "escaped.html"
    report.export(path, "html")
    html = path.read_text(encoding="utf-8")
    assert "<script>" not in html
    assert "&lt;script&gt;" in html


def test_report_comparison_and_history(tmp_path):
    first = storq.ReportBuilder("First", report_type="recovery").build()
    first.add_finding(
        storq.ReportFinding("f1", "Missing transaction", severity="high", source_module="recovery"))
    second = storq.ReportBuilder("Second", report_type="recovery").build()
    comparison = storq.compare_reports(first, second)
    assert comparison.resolved_findings == ["Missing transaction"]

    history = storq.report_history(tmp_path)
    history.add(first)
    assert history.get(first.metadata.report_id)["metadata"]["title"] == "First"
