"""Bounded-memory statistics with explicit denominators."""

from collections import Counter, defaultdict


class Totals:
    def __init__(self):
        self.count = self.raw = self.text = 0
        self.statuses = Counter()
        self.methods = Counter()
        self.warnings = Counter()
        self.features = Counter()
        self.metadata = Counter()
        self.http_statuses = Counter()
        self.raw_min = self.raw_max = None

    def add(self, record):
        self.count += 1
        raw = record["raw_utf8_bytes"]
        self.raw += raw
        self.text += record["text_utf8_bytes"]
        self.raw_min = raw if self.raw_min is None else min(raw, self.raw_min)
        self.raw_max = raw if self.raw_max is None else max(raw, self.raw_max)
        self.statuses[record["status"]] += 1
        self.methods[record.get("extraction_method") or "none"] += 1
        self.http_statuses[str(record.get("last_http_status"))] += 1
        self.warnings.update(record.get("warnings", []))
        self.features.update(k for k, v in record.get("features", {}).items() if v)
        self.metadata.update(k for k in ("title", "language", "author", "published_at") if record.get(k))

    def as_dict(self):
        return dict(count=self.count, raw_utf8_bytes=self.raw, text_utf8_bytes=self.text,
                    average_raw_utf8_bytes=self.raw / self.count if self.count else None,
                    average_text_utf8_bytes=self.text / self.count if self.count else None,
                    average_successful_text_utf8_bytes=self.text / self.statuses["ok"] if self.statuses["ok"] else None,
                    text_to_raw_ratio=self.text / self.raw if self.raw else None,
                    min_raw_utf8_bytes=self.raw_min, max_raw_utf8_bytes=self.raw_max,
                    statuses=dict(self.statuses), extraction_methods=dict(self.methods),
                    warnings=dict(self.warnings), features=dict(self.features),
                    metadata_present=dict(self.metadata), http_statuses=dict(self.http_statuses))


class Statistics:
    def __init__(self):
        self.total = Totals()
        self.sources = defaultdict(Totals)
        self.field_types = defaultdict(Counter)

    def add(self, original, record):
        self.total.add(record)
        self.sources[record["source"]].add(record)
        for key, value in original.items():
            self.field_types[key][type(value).__name__] += 1

    def as_dict(self):
        return dict(total=self.total.as_dict(),
                    sources={s: t.as_dict() for s, t in sorted(self.sources.items())},
                    fields={k: {"types": dict(v), "missing": self.total.count - sum(v.values())}
                            for k, v in sorted(self.field_types.items())})
