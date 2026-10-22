import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from uuid import uuid4

from bson import ObjectId
from pymongo import MongoClient


@unittest.skipUnless(os.environ.get("TOKENIZER_BINARY"), "Set TOKENIZER_BINARY for MongoDB integration tests")
class RepositoryTests(unittest.TestCase):
    def setUp(self):
        self.binary = str(Path(os.environ["TOKENIZER_BINARY"]).resolve())
        self.uri = os.environ.get("TOKENIZER_TEST_URI", "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000")
        self.client = MongoClient(self.uri)
        self.database = "tokenizer_test_" + uuid4().hex
        self.db = self.client[self.database]
        self.directory = tempfile.TemporaryDirectory()
        self.db.corpus_runs.insert_one({"_id": "corpus", "state": "completed"})
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(self.client.close)
        self.addCleanup(self.client.drop_database, self.database)

    def document(self, **fields):
        record = dict(run_id="corpus", document_id=ObjectId(), status="ok",
                      source="stopgame", url="https://example.test/article", title="ЁЖ", text="PS5 C++")
        record.update(fields)
        self.db.corpus_documents.insert_one(record)
        return record["document_id"]

    def run_tokenizer(self, run="tokens", *args, success=True):
        result = subprocess.run([self.binary, "--uri", self.uri, "--database", self.database,
                                 "--corpus-run", "corpus", "--run-id", run,
                                 "--output", self.directory.name, *args], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_tokens_statistics_and_immutable_input(self):
        identity = self.document()
        before = self.db.corpus_documents.find_one({"document_id": identity})
        self.document(status="empty")
        self.run_tokenizer()
        run = self.db.tokenization_runs.find_one({"_id": "tokens"})
        self.assertEqual(run["state"], "completed")
        stats = run["statistics"]
        self.assertEqual(stats["total"]["tokens"], 3)
        self.assertEqual(stats["total"]["token_codepoints"], 6)
        self.assertEqual(stats["total"]["average_token_length"], 2)
        self.assertEqual(stats["total"]["input_utf8_bytes"], len("ЁЖPS5 C++".encode()))
        self.assertEqual(stats["selected_documents"], 1)
        body = self.db.tokenized_documents.find_one({"field": "text"})
        self.assertEqual(body["document_id"], identity)
        self.assertEqual([t["normalized"] for t in body["tokens"]], ["ps5", "c"])
        self.assertEqual(body["tokens"][1]["byte_begin"], 4)
        self.assertEqual(self.db.corpus_documents.find_one({"document_id": identity}), before)
        report = Path(self.directory.name) / "tokens"
        self.assertTrue((report / "report.md").exists())
        self.assertEqual(json.loads((report / "statistics.json").read_text())["statistics"], stats)

    def test_bad_document_is_recorded_without_partial_fields(self):
        self.document(text=123)
        self.run_tokenizer()
        run = self.db.tokenization_runs.find_one({"_id": "tokens"})
        self.assertEqual(run["statistics"]["error_documents"], 1)
        self.assertEqual(run["statistics"]["total"]["tokens"], 0)
        self.assertEqual(self.db.tokenized_documents.count_documents({}), 1)
        self.assertEqual(self.db.tokenized_documents.find_one()["status"], "error")

    def test_empty_corpus(self):
        self.run_tokenizer()
        stats = self.db.tokenization_runs.find_one()["statistics"]
        self.assertIsNone(stats["total"]["average_token_length"])
        self.assertIsNone(stats["total"]["core_ms_per_kib"])

    def test_unknown_source_and_invalid_cli(self):
        self.document()
        self.run_tokenizer("unknown", "--source", "missing", success=False)
        self.run_tokenizer("bad-limit", "--limit", "-1", success=False)
        self.run_tokenizer("../unsafe", success=False)
        self.assertEqual(self.db.tokenization_runs.count_documents({}), 0)

    def test_unacknowledged_writes_are_rejected(self):
        self.document()
        uri = self.uri + ("&" if "?" in self.uri else "?") + "w=0"
        self.run_tokenizer("unacknowledged", "--uri", uri, success=False)
        self.assertEqual(self.db.tokenization_runs.count_documents({}), 0)

    def test_oversized_token_rejects_entire_document(self):
        self.document(text="a" * (4 * 1024 * 1024))
        self.run_tokenizer()
        record = self.db.tokenized_documents.find_one()
        self.assertEqual(record["status"], "error")
        self.assertIn("4 MiB", record["error"])
        self.assertEqual(self.db.tokenized_documents.count_documents({}), 1)

    def test_filters_limit_duplicate_and_unfinished_run(self):
        self.document(source="vgtimes")
        self.document()
        self.document()
        self.run_tokenizer("one", "--source", "stopgame", "--limit", "1")
        stats = self.db.tokenization_runs.find_one({"_id": "one"})["statistics"]
        self.assertEqual(stats["selected_documents"], 1)
        self.assertEqual(self.db.tokenized_documents.distinct("source", {"tokenization_run_id": "one"}), ["stopgame"])
        self.run_tokenizer("one", success=False)
        self.assertEqual(self.db.tokenization_runs.find_one({"_id": "one"})["state"], "completed")
        self.run_tokenizer("one", "--output", str(Path(self.directory.name) / "other"), success=False)
        self.assertEqual(self.db.tokenization_runs.find_one({"_id": "one"})["state"], "completed")
        self.db.corpus_runs.update_one({"_id": "corpus"}, {"$set": {"state": "running"}})
        self.run_tokenizer("unfinished", success=False)
        self.assertIsNone(self.db.tokenization_runs.find_one({"_id": "unfinished"}))

    def test_chunking_preserves_continuous_positions(self):
        identity = self.document(title=None, text="слово " * 80000)
        self.run_tokenizer()
        parts = list(self.db.tokenized_documents.find({"field": "text"}).sort("part", 1))
        self.assertGreater(len(parts), 1)
        tokens = [token for part in parts for token in part["tokens"]]
        self.assertEqual(len(tokens), 80000)
        self.assertEqual([token["position"] for token in tokens], list(range(80000)))
        self.assertTrue(all(part["document_id"] == identity for part in parts))

    def test_write_report_failure_marks_run_failed(self):
        self.document()
        self.run_tokenizer("failure", "--output", "/proc/tokenizer-test-output", success=False)
        self.assertEqual(self.db.tokenization_runs.find_one({"_id": "failure"})["state"], "failed")

    def test_default_selects_latest_completed_corpus(self):
        self.document()
        self.db.corpus_runs.insert_many([
            {"_id": "newer", "state": "completed", "finished_at": "2026-10-23"},
            {"_id": "incomplete", "state": "running", "finished_at": "2026-10-24"},
        ])
        identity = self.document(run_id="newer", text="Новый текст")
        result = subprocess.run([self.binary, "--uri", self.uri, "--database", self.database,
                                 "--run-id", "auto", "--output", self.directory.name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        run = self.db.tokenization_runs.find_one({"_id": "auto"})
        self.assertEqual(run["corpus_run_id"], "newer")
        self.assertEqual(self.db.tokenized_documents.distinct("document_id"), [identity])


if __name__ == "__main__":
    unittest.main()
