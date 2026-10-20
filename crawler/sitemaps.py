import gzip
from defusedxml.ElementTree import iterparse


def entries(stream, max_bytes: int):
    stream.seek(0)
    compressed = stream.read(2) == b"\x1f\x8b"
    stream.seek(0)
    reader = gzip.GzipFile(fileobj=stream, mode="rb") if compressed else stream

    class LimitedReader:
        def __init__(self):
            self.count = 0

        def read(self, size=-1):
            data = reader.read(min(size if size >= 0 else 65536, 65536))
            self.count += len(data)
            if self.count > max_bytes:
                raise ValueError("Decompressed sitemap exceeds size limit")
            return data

    root = None
    for event, element in iterparse(LimitedReader(), events=("start", "end")):
        tag = element.tag.rsplit("}", 1)[-1]
        if root is None:
            root = element
            if tag not in ("urlset", "sitemapindex"):
                raise ValueError("Not a sitemap XML document")
        if event == "end" and tag in ("url", "sitemap"):
            values = {child.tag.rsplit("}", 1)[-1]: child.text for child in element}
            if values.get("loc"):
                yield ("sitemap" if tag == "sitemap" else "page", values["loc"].strip(), values.get("lastmod"))
            root.clear()
