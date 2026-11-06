import html
from pathlib import Path

from .base import ReportExporter
from ..models import Report, ReportExportResult


class HTMLExporter(ReportExporter):
    def export(self, report: Report, path: Path) -> ReportExportResult:
        self.validate_path(path)
        def e(value) -> str:
            return html.escape(str(value), quote=True)

        sections = []
        for section in report.sections:
            items = "".join(
                f"<dt>{e(key)}</dt><dd>{e(value)}</dd>"
                for key, value in section.content.items()
            )
            tables = []
            for table in section.tables:
                head = "".join(f"<th>{e(column)}</th>" for column in table.columns)
                body = "".join(
                    "<tr>" + "".join(f"<td>{e(row.get(column, ''))}</td>" for column in table.columns) + "</tr>"
                    for row in table.rows[:10000]
                )
                tables.append(f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>")
            sections.append(
                f"<section><h2>{e(section.title)}</h2><dl>{items}</dl>{''.join(tables)}</section>"
            )
        findings = "".join(
            f"<li class=\"finding-{e(f.severity)}\"><strong>{e(f.title)}</strong>: {e(f.description)} "
            f"<span>[{e(f.status)}, {e(f.severity)}]</span></li>"
            for f in report.findings
        )
        document = (
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<style>body{font-family:system-ui;margin:2rem;color:#222} section{margin:2rem 0}"
            "table{border-collapse:collapse;width:100%}th,td{border:1px solid #ccc;padding:.4rem}"
            "dl{display:grid;grid-template-columns:max-content 1fr}dt{font-weight:bold;margin-right:1rem}"
            "@media(max-width:600px){body{margin:.5rem}}</style></head><body>"
            f"<h1>{e(report.metadata.title)}</h1><p>{e(report.description)}</p>"
            f"<p>Generated {e(report.metadata.generated_at.isoformat())}</p>"
            f"<h2>Findings</h2><ul>{findings}</ul>{''.join(sections)}</body></html>"
        )
        path.write_text(document, encoding="utf-8")
        return ReportExportResult(str(path), "html", path.stat().st_size)
