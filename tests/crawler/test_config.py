import tempfile
from pathlib import Path

import yaml

from crawler.config import load_config
from tests.support import UnitTestCase, config


class ConfigTests(UnitTestCase):
    def load(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yaml"
            path.write_text(yaml.safe_dump(data), encoding="utf-8")
            return load_config(path)

    def test_defaults_and_domain_normalization(self):
        data = config()
        del data["logic"]
        data["sources"] = [{"name": "example", "domain": "EXAMPLE.TEST"}]
        result = self.load(data)
        self.assertEqual(result["logic"]["target_documents"], 1000)
        self.assertEqual(result["sources"][0]["domain"], "example.test")
        self.assertEqual(result["sources"][0]["concurrency"], 2)
        self.assertEqual(result["sources"][0]["sitemap_urls"], [])

    def test_invalid_top_level_and_database(self):
        for data in (None, [], {}, {"db": []}, {"db": {"uri": "http://db", "database": "x"}}):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.load(data)

    def test_numeric_logic_validation(self):
        for key in config()["logic"]:
            if key == "user_agent":
                continue
            for value in (0, -1, True, "1", float("nan"), float("inf")):
                data = config()
                data["logic"][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    self.load(data)
        for key in ("target_documents", "max_retries", "max_html_bytes", "max_sitemap_bytes"):
            data = config()
            data["logic"][key] = 1.5
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.load(data)

    def test_source_validation(self):
        for key, value in (("name", ""), ("domain", "example.test/path"), ("concurrency", True),
                           ("concurrency", 0), ("delay_seconds", -1), ("delay_seconds", True),
                           ("sitemap_urls", "https://example.test/x"),
                           ("sitemap_urls", ["https://elsewhere.test/x"])):
            data = config()
            data["sources"][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.load(data)
        data = config()
        data["sources"].append(dict(data["sources"][0]))
        with self.assertRaises(ValueError):
            self.load(data)

    def test_html_bson_budget_and_user_agent(self):
        for key, value in (("max_html_bytes", 8000001), ("user_agent", " "), ("user_agent", 5)):
            data = config()
            data["logic"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.load(data)
