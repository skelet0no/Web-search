import copy
import tempfile
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from corpus.pipeline import run_analysis
from tests.support import Cursor, UnitTestCase, config


class PipelineTests(UnitTestCase):
    def setUp(self):
        super().setUp()
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        self.rows = [{"_id": "id", "source": "example", "url": "https://example.test/a",
                      "raw_html": "<article><h1>Title</h1><p>Body</p></article>", "content_hash": "hash"}]
        self.repo = Mock()
        self.repo.db = SimpleNamespace(documents=Mock())
        self.repo.db.documents.count_documents.return_value = 1
        self.repo.documents.side_effect = lambda *a: Cursor(self.rows)
        self.repo.sample_documents.side_effect = lambda *a: Cursor(self.rows)
        self.check = Mock()

        @contextmanager
        def frozen():
            yield self.check

        self.repo.frozen.side_effect = frozen
        self.repo.create_run.side_effect = lambda summary: setattr(self, "created", copy.deepcopy(summary))
        self.repo.update_run.side_effect = lambda summary: setattr(self, "updated", copy.deepcopy(summary))

    def run_analysis(self, **kwargs):
        options = dict(run_id="unit", output=self.output)
        options.update(kwargs)
        return run_analysis(self.repo, config(), **options)

    def test_extract_saves_records_snapshot_metadata_and_reports(self):
        summary = self.run_analysis()
        self.assertEqual(self.created["state"], "running")
        self.assertEqual(self.updated["state"], "completed")
        self.assertEqual(summary["processed_documents"], 1)
        self.assertEqual(summary["snapshot_mode"], "crawler_lease")
        saved = self.repo.save_document.call_args.args[0]
        self.assertEqual(saved["document_id"], "id")
        self.assertEqual(saved["source_content_hash"], "hash")
        self.assertEqual(saved["text"], "Body")
        self.assertTrue((self.output / "unit/report.md").is_file())
        self.assertTrue((self.output / "unit/samples.csv").is_file())
        self.check.assert_called()

    def test_inspect_never_writes_database_or_acquires_lock(self):
        summary = self.run_analysis(mode="inspect")
        self.assertEqual(summary["selection"]["limit"], 100)
        self.assertEqual(summary["snapshot_mode"], "live_preview")
        for name in ("frozen", "setup", "save_document", "create_run", "update_run"):
            getattr(self.repo, name).assert_not_called()

    def test_balanced_sampling_and_url_selection(self):
        data = config()
        data["sources"].append({"name": "second"})
        result = run_analysis(self.repo, data, run_id="unit", output=self.output,
                              mode="inspect", limit=4, url_pattern="/games/")
        self.repo.sample_documents.assert_called_once()
        self.repo.documents.assert_not_called()
        query = self.repo.sample_documents.call_args.args[0]
        self.assertEqual(query["source"]["$in"], ["example", "second"])
        self.assertEqual(query["url"]["$regex"], "/games/")
        self.assertIn("round-robin", result["selection"]["order"])

    def test_extractor_error_becomes_error_record(self):
        with patch("corpus.pipeline.extract_document", side_effect=ValueError("bad html")):
            summary = self.run_analysis()
        self.assertEqual(summary["state"], "completed")
        saved = self.repo.save_document.call_args.args[0]
        self.assertEqual(saved["status"], "error")
        self.assertEqual(saved["text_utf8_bytes"], 0)
        self.assertIn("extraction_error:ValueError", saved["warnings"])

    def test_snapshot_count_change_fails_and_removes_complete_report(self):
        self.repo.db.documents.count_documents.return_value = 2
        with self.assertRaisesRegex(RuntimeError, "snapshot is incomplete"):
            self.run_analysis()
        self.assertEqual(self.updated["state"], "failed")
        self.assertFalse((self.output / "unit/report.md").exists())

    def test_lease_loss_and_report_failure_do_not_publish_completed_run(self):
        self.check.side_effect = RuntimeError("lost")
        with self.assertRaisesRegex(RuntimeError, "lost"):
            self.run_analysis()
        self.assertEqual(self.updated["state"], "failed")
        self.check.side_effect = None
        with patch("corpus.pipeline.write_report", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.run_analysis(run_id="another")
        self.assertEqual(self.updated["state"], "failed")

    def test_existing_output_and_invalid_options_rejected_before_database_write(self):
        for options in ({"run_id": "../escape"}, {"limit": 0}, {"batch_size": 0},
                        {"samples_per_source": 1}, {"mode": "bad"},
                        {"sources": ["unknown"]}, {"url_pattern": "["}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.run_analysis(**options)
        (self.output / "unit").mkdir()
        with self.assertRaises(ValueError):
            self.run_analysis()
        self.repo.create_run.assert_not_called()

    def test_interrupt_records_failure_and_is_reraised(self):
        self.repo.save_document.side_effect = KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            self.run_analysis()
        self.assertEqual(self.updated["error"], "KeyboardInterrupt")
