from dataclasses import dataclass, field
from typing import Any


@dataclass
class ReportingConfig:
    theme: str = "light"
    include_evidence: bool = True
    include_raw_data: bool = False
    interactive_charts: bool = False
    width: int = 1000
    height: int = 600
    dpi: int = 150
    page_size: str = "A4"
    max_displayed_records: int = 10000
    chart_backend: str = "matplotlib"
    html_self_contained: bool = True
    chart_settings: dict[str, Any] = field(default_factory=dict)

    def validate(self) -> None:
        if self.theme not in {"light", "dark"}:
            raise ValueError("theme must be light or dark")
        if self.width <= 0 or self.height <= 0 or self.dpi <= 0:
            raise ValueError("chart dimensions and DPI must be positive")
        if self.max_displayed_records < 0:
            raise ValueError("maximum displayed records cannot be negative")
