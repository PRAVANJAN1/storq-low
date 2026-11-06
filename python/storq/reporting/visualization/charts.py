def page_distribution(report):
    counts = {}
    for section in report.sections:
        for key, value in section.content.items():
            if key in {"leaf_table_pages", "interior_table_pages", "leaf_index_pages",
                       "interior_index_pages", "overflow_pages", "unclassified_pages"}:
                if isinstance(value, (int, float)):
                    counts[key] = value
    if not counts:
        return {}, []
    return counts, list(counts)
