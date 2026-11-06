from .builder import ReportBuilder
from .registry import registry
from .templates import TEMPLATES


class ReportingEngine:
    def create_report(self, source=None, template: str = "comprehensive", title: str | None = None):
        if title is None:
            title = template.replace("_", " ").title()
        report_type = template if template in TEMPLATES else "comprehensive"
        builder = ReportBuilder(title, report_type=report_type)
        if source is not None:
            source_type = type(source).__name__
            module = "workflow" if source_type == "WorkflowResult" else "module_result"
            builder.add(source, module)
        report = builder.build()
        return report

    create_report_from_workflow = create_report
