from types import SimpleNamespace
from unittest.mock import Mock, patch

from pymongo.errors import DuplicateKeyError

from corpus.repository import Repository
from tests.support import Cursor, UnitTestCase, config


class RepositoryTests(UnitTestCase):
    def setUp(self):
        super().setUp()

        self.repo = object.__new__(Repository)
        self.repo.db = SimpleNamespace(documents=Mock(), corpus_documents=Mock(), corpus_runs=Mock(), state=Mock())
        self.repo.client = Mock()

    def test_connection_and_close(self):
        with patch("corpus.repository.MongoClient") as client:
            repo = Repository(config())
            repo.close()
        client.assert_called_once_with("mongodb://localhost:27017", serverSelectionTimeoutMS=10000)
        client.return_value.close.assert_called_once()

    def test_unique_run_document_index_and_persistence(self):
        self.repo.setup()
        self.repo.db.corpus_documents.create_index.assert_called_once_with([("run_id", 1), ("document_id", 1)], unique=True)
        self.repo.create_run({"run_id": "run", "state": "running"})
        self.repo.db.corpus_runs.insert_one.assert_called_once_with({"_id": "run", "run_id": "run", "state": "running"})
        self.repo.save_document({"text": "Body"})
        self.repo.db.corpus_documents.insert_one.assert_called_once_with({"text": "Body"})
        self.repo.update_run({"run_id": "run", "state": "completed"})
        self.repo.db.corpus_runs.update_one.assert_called_once_with({"_id": "run"}, {"$set": {"run_id": "run", "state": "completed"}})

    def test_duplicate_run_is_not_overwritten(self):
        self.repo.db.corpus_runs.insert_one.side_effect = DuplicateKeyError("duplicate")
        with self.assertRaisesRegex(ValueError, "immutable"):
            self.repo.create_run({"run_id": "run"})

    def test_document_projection_sort_batch_and_limit(self):
        cursor = Cursor([{"_id": 1}])
        self.repo.db.documents.find.return_value = cursor
        self.assertIs(self.repo.documents({"source": "a"}, 7, 20), cursor)
        self.assertEqual(cursor.sort_args, ("_id", 1))
        self.assertEqual(cursor.batch, 20)
        self.assertEqual(cursor.maximum, 7)
        self.assertEqual(set(self.repo.db.documents.find.call_args.args[1]), {"url", "source", "raw_html", "content_hash"})

    def test_round_robin_handles_exhausted_source_and_closes_cursors(self):
        first, second = Cursor([1, 3, 5]), Cursor([2])
        with patch.object(self.repo, "documents", side_effect=[first, second]):
            with self.repo.sample_documents({}, ["a", "b"], 4, 10) as rows:
                self.assertEqual(list(rows), [1, 2, 3, 5])
        self.assertTrue(first.closed and second.closed)

    def test_lock_contention_does_not_start_renewal_thread(self):
        self.repo.db.state.find_one_and_update.side_effect = DuplicateKeyError("busy")
        with patch("corpus.repository.threading.Thread") as thread:
            with self.assertRaisesRegex(ValueError, "Crawler"):
                with self.repo.frozen():
                    self.fail("Lock must not be acquired")
        thread.assert_not_called()

    def test_lock_checks_current_owner_and_releases_on_error(self):
        self.repo.db.state.find_one_and_update.return_value = {"_id": "runner"}
        self.repo.db.state.find_one.return_value = {"_id": "runner"}
        with patch("corpus.repository.threading.Thread") as thread:
            with self.assertRaisesRegex(RuntimeError, "body failure"):
                with self.repo.frozen() as check:
                    check()
                    self.assertTrue(self.repo.db.state.find_one.call_args.args[0]["owner"].startswith("corpus-"))
                    raise RuntimeError("body failure")
        thread.return_value.start.assert_called_once()
        thread.return_value.join.assert_called_once()
        self.assertTrue(self.repo.db.state.delete_one.call_args.args[0]["owner"].startswith("corpus-"))

    def test_expired_lease_is_not_accepted_as_snapshot(self):
        self.repo.db.state.find_one.return_value = None
        with patch("corpus.repository.threading.Thread"):
            with self.repo.frozen() as check:
                with self.assertRaisesRegex(RuntimeError, "expired"):
                    check()

    def test_renewal_uses_owner_and_detects_lost_lease(self):
        for matched in (1, 0):
            with self.subTest(matched=matched):
                stop, lost = Mock(), Mock()
                stop.wait.side_effect = [False, True]
                lost.is_set.return_value = False
                lost.set.side_effect = lambda: setattr(lost.is_set, "return_value", True)
                self.repo.db.state.find_one.return_value = {"_id": "runner"}
                self.repo.db.state.update_one.return_value = SimpleNamespace(matched_count=matched)
                with patch("corpus.repository.threading.Event", side_effect=[stop, lost]), patch("corpus.repository.threading.Thread") as thread:
                    with self.repo.frozen() as check:
                        thread.call_args.kwargs["target"]()
                        query = self.repo.db.state.update_one.call_args.args[0]
                        self.assertIn("owner", query)
                        self.assertIn("$gt", query["until"])
                        if matched:
                            check()
                        else:
                            with self.assertRaisesRegex(RuntimeError, "lost"):
                                check()
