from collections import Counter, defaultdict


class Totals:
    def __init__(self):
        self.count = self.raw = self.text = 0
        self.successful = 0
        self.features = Counter()
        self.metadata = Counter()

    def add(self, record):
        self.count += 1
        self.raw += record["raw_utf8_bytes"]
        self.text += record["text_utf8_bytes"]
        self.successful += record["status"] == "ok"
        self.features.update(k for k, v in record.get("features", {}).items() if v)
        self.metadata.update(k for k in ("title", "language", "author", "published_at") if record.get(k))

    def as_dict(self):
        return dict(count=self.count, raw_utf8_bytes=self.raw, text_utf8_bytes=self.text,
                    average_raw_utf8_bytes=self.raw / self.count if self.count else None,
                    average_text_utf8_bytes=self.text / self.count if self.count else None,
                    average_successful_text_utf8_bytes=self.text / self.successful if self.successful else None,
                    features=dict(self.features), metadata_present=dict(self.metadata))


class Statistics:
    def __init__(self):
        self.total = Totals()
        self.sources = defaultdict(Totals)

    def add(self, record):
        self.total.add(record)
        self.sources[record["source"]].add(record)

    def as_dict(self):
        return dict(total=self.total.as_dict(),
                    sources={s: t.as_dict() for s, t in sorted(self.sources.items())})
