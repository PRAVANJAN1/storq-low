from pathlib import Path

from .charts import page_distribution


class OptionalDependencyError(ImportError):
    pass


class VisualizationEngine:
    def render(self, report, chart_type: str, output: str | Path | None = None, interactive: bool = False):
        values, labels = self._data(report, chart_type)
        if interactive:
            try:
                import plotly.graph_objects as go
            except ImportError as error:
                raise OptionalDependencyError(
                    "Plotly charts require storq[visualization]"
                ) from error
            figure = go.Figure(data=[go.Bar(x=labels, y=list(values.values()))])
            figure.update_layout(title=chart_type.replace("_", " ").title())
            if output is not None:
                Path(output).write_text(figure.to_html(include_plotlyjs="inline"), encoding="utf-8")
            return figure
        try:
            import matplotlib.pyplot as plt
        except ImportError as error:
            raise OptionalDependencyError(
                "Static charts require matplotlib; install storq[visualization]"
            ) from error
        figure, axis = plt.subplots(figsize=(10, 6))
        axis.bar(labels, list(values.values()))
        axis.set_title(chart_type.replace("_", " ").title())
        figure.tight_layout()
        if output is not None:
            figure.savefig(output, dpi=150)
            plt.close(figure)
            return Path(output)
        return figure

    def _data(self, report, chart_type: str):
        if chart_type == "page_distribution":
            return page_distribution(report)
        data = {}
        for section in report.sections:
            for key, value in section.content.items():
                if isinstance(value, (int, float)):
                    data[key] = value
        return data, list(data)
