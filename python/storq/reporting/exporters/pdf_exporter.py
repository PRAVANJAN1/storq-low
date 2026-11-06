from pathlib import Path

from .base import ReportExporter
from ..models import Report, ReportExportResult


class PDFExporter(ReportExporter):
    def export(self, report: Report, path: Path) -> ReportExportResult:
        self.validate_path(path)
        try:
            from reportlab.lib import colors
            from reportlab.lib.pagesizes import A4, letter
            from reportlab.lib.styles import getSampleStyleSheet
            from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle
        except ImportError as error:
            raise ImportError(
                "PDF export requires the optional reportlab dependency; install storq[full-reporting]"
            ) from error
        pagesize = letter if getattr(report.metadata, "page_size", "A4").lower() == "letter" else A4
        styles = getSampleStyleSheet()
        document = SimpleDocTemplate(str(path), pagesize=pagesize)
        flowables = [
            Paragraph(report.metadata.title, styles["Title"]),
            Paragraph(report.description or "", styles["BodyText"]),
            Spacer(1, 12),
        ]
        for section in report.sections:
            flowables.append(Paragraph(section.title, styles["Heading2"]))
            for key, value in section.content.items():
                flowables.append(Paragraph(f"<b>{key}</b>: {value}", styles["BodyText"]))
            for table in section.tables[:50]:
                data = [[table.title] + [""] * max(0, len(table.columns) - 1)]
                data.append(table.columns)
                data.extend([[str(row.get(column, "")) for column in table.columns] for row in table.rows[:5000]])
                table_flow = Table(data, repeatRows=2)
                table_flow.setStyle(TableStyle([
                    ("BACKGROUND", (0, 1), (-1, 1), colors.lightgrey),
                    ("GRID", (0, 0), (-1, -1), 0.25, colors.grey),
                ]))
                flowables.append(table_flow)
        document.build(flowables)
        return ReportExportResult(str(path), "pdf", path.stat().st_size)
