import csv
import tempfile
from pathlib import Path

import yaml

from corpus.config import load_config
from corpus.report import Samples, cell, write_report
from corpus.stats import Statistics, Totals
from tests.support import UnitTestCase, config


def record(key="id", source="example", raw=100, text="Игра", status="ok"):
    return dict(document_id=key, source=source, raw_utf8_bytes=raw,
                text_utf8_bytes=len(text.encode()), text=text, title="Title", language="ru",
                author=None, published_at=None, status=status, features={"article": True, "table": False},
                url="https://example.test/" + str(key), page_type="article")


class CorpusConfigTests(UnitTestCase):
    def test_uri_override_and_source_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yaml"
            path.write_text(yaml.safe_dump({"db": config()["db"]}), encoding="utf-8")
            result = load_config(path, "mongodb+srv://cluster.test/db")
        self.assertEqual(result["db"]["uri"], "mongodb+srv://cluster.test/db")
        self.assertEqual(result["sources"], [])

    def test_invalid_configuration(self):
        cases = (None, [], {"db": []}, {"db": {"uri": "http://db", "database": "x"}},
                 {"db": {"uri": "mongodb://db", "database": " "}},
                 {"db": config()["db"], "sources": [{"name": 4}]})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yaml"
            for data in cases:
                path.write_text(yaml.safe_dump(data), encoding="utf-8")
                with self.subTest(data=data), self.assertRaises(ValueError):
                    load_config(path)


class StatisticsTests(UnitTestCase):
    def test_empty_denominators_are_null(self):
        result = Totals().as_dict()
        self.assertEqual(result["count"], 0)
        self.assertIsNone(result["average_raw_utf8_bytes"])
        self.assertIsNone(result["average_successful_text_utf8_bytes"])

    def test_success_errors_feature_presence_and_sources(self):
        stats = Statistics()
        stats.add(record(source="b", raw=100, text="AB"))
        stats.add(record(source="a", raw=200, text="", status="error"))
        result = stats.as_dict()
        self.assertEqual(list(result["sources"]), ["a", "b"])
        total = result["total"]
        self.assertEqual(total["count"], 2)
        self.assertEqual(total["average_raw_utf8_bytes"], 150)
        self.assertEqual(total["average_text_utf8_bytes"], 1)
        self.assertEqual(total["average_successful_text_utf8_bytes"], 2)
        self.assertEqual(total["features"], {"article": 2})
        self.assertEqual(total["metadata_present"], {"title": 2, "language": 2})


class ReportTests(UnitTestCase):
    def test_samples_are_bounded_keep_extremes_and_order_independent(self):
        rows = [record(key=str(i), raw=i + 1) for i in range(20)]
        first, second = Samples(5), Samples(5)
        for row in rows:
            first.add({"raw_html": "<html>"}, row)
        for row in reversed(rows):
            second.add({"raw_html": "<html>"}, row)
        self.assertEqual(set(first.groups["example"]), set(second.groups["example"]))
        self.assertEqual(len(first.groups["example"]), 5)
        self.assertTrue({"0", "19"} <= first.groups["example"].keys())

    def test_export_uses_safe_paths_and_preserves_utf8(self):
        samples = Samples(2)
        row = record(key="../../escape", source="source/../name")
        samples.add({"raw_html": "<p>Игра</p>"}, row)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = samples.export(root)
            self.assertEqual(len(rows), 1)
            for key in ("html_path", "text_path"):
                path = root / rows[0][key]
                self.assertEqual(path.parent, root / "samples")
                self.assertTrue(path.is_file())
            self.assertEqual((root / rows[0]["text_path"]).read_text(encoding="utf-8"), "Игра")
            with (root / "samples.csv").open(encoding="utf-8", newline="") as stream:
                self.assertEqual(list(csv.DictReader(stream))[0]["document_id"], "../../escape")

    def test_report_contains_statistics_and_escapes_markdown(self):
        stats = Statistics()
        stats.add(record())
        summary = dict(run_id="unit", mode="inspect", state="completed", extractor_version="test",
                       is_sample=True, started_at="2026-01-01", snapshot_mode="live_preview",
                       selection={"sources": ["example"], "limit": 1}, statistics=stats.as_dict(), samples=[])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_report(root, summary)
            report = (root / "report.md").read_text(encoding="utf-8")
            self.assertIn("unit", report)
            self.assertIn("example", report)
        self.assertNotIn("\n", cell("a|b\nc"))
        self.assertIn("\\|", cell("a|b"))
