from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

from pymongo.errors import DuplicateKeyError

from crawler.storage import Store, connect
from tests.support import AsyncUnitTestCase, Cursor, config


class StorageTests(AsyncUnitTestCase):
    def setUp(self):
        super().setUp()
        self.db = SimpleNamespace(documents=Mock(), crawl_queue=Mock(), state=Mock())
        self.store = Store(self.db, lease_seconds=30)

        async def invoke(function, *args, **kwargs):
            return function(*args, **kwargs)

        self.store.call = AsyncMock(side_effect=invoke)

    async def test_indexes_and_owner_scoped_release(self):
        await self.store.setup()
        self.db.documents.create_index.assert_any_call("url", unique=True)
        self.db.crawl_queue.create_index.assert_any_call("url", unique=True)
        await self.store.release()
        self.db.state.delete_one.assert_called_once_with({"_id": "runner", "owner": self.store.owner})

    async def test_lock_acquisition_contention_and_no_match(self):
        self.db.state.find_one_and_update.return_value = {"owner": self.store.owner}
        with patch("crawler.storage.time.time", return_value=100):
            self.assertTrue(await self.store.lock())
        query, update = self.db.state.find_one_and_update.call_args.args
        self.assertEqual(query["$or"], [{"until": {"$lt": 100}}, {"owner": self.store.owner}])
        self.assertEqual(update["$set"]["until"], 130)
        self.db.state.find_one_and_update.side_effect = DuplicateKeyError("busy")
        self.assertFalse(await self.store.lock())
        self.db.state.find_one_and_update.side_effect = None
        self.db.state.find_one_and_update.return_value = None
        self.assertFalse(await self.store.lock())

    async def test_empty_enqueue_does_not_write(self):
        await self.store.enqueue("example", [])
        self.db.crawl_queue.bulk_write.assert_not_called()

    async def test_enqueue_is_idempotent_and_retains_lastmod(self):
        await self.store.enqueue("example", [("page", "https://example.test/a", "today")])
        operations = self.db.crawl_queue.bulk_write.call_args.args[0]

        self.assertEqual(operations[0]._filter, {"url": "https://example.test/a"})
        self.assertEqual(operations[0]._doc["$setOnInsert"]["lastmod"], "today")
        self.assertTrue(operations[0]._upsert)
        self.assertFalse(self.db.crawl_queue.bulk_write.call_args.kwargs["ordered"])

    async def test_claim_reclaims_expired_lease(self):
        with patch("crawler.storage.time.time", return_value=100):
            await self.store.claim("example", "page")
        query, update = self.db.crawl_queue.find_one_and_update.call_args.args
        self.assertEqual(query["$or"][1], {"status": "running", "lease_until": {"$lte": 100}})
        self.assertEqual(update["$set"]["owner"], self.store.owner)
        self.assertEqual(update["$set"]["lease_until"], 130)

    async def test_finish_and_retry_require_current_owner(self):
        job = {"_id": "job", "attempts": 1}
        await self.store.finish(job, last_error="skip")
        query, update = self.db.crawl_queue.update_one.call_args.args
        self.assertEqual(query, {"_id": "job", "owner": self.store.owner})
        self.assertEqual(update["$set"], {"status": "done", "last_error": "skip"})
        for attempts, expected in ((0, "pending"), (2, "failed")):
            with patch("crawler.storage.time.time", return_value=100):
                await self.store.fail({"_id": "job", "attempts": attempts}, ValueError("oops"), 20, 3)
            fields = self.db.crawl_queue.update_one.call_args.args[1]["$set"]
            self.assertEqual(fields["status"], expected)
            self.assertEqual(fields["attempts"], attempts + 1)
            self.assertEqual(fields["next_attempt"], 120)

    async def test_counts_remaining_and_due_order(self):
        self.db.documents.aggregate.return_value = [{"_id": "example", "count": 3}]
        self.assertEqual(await self.store.counts(), {"example": 3})
        self.db.crawl_queue.find_one.return_value = {"_id": "job"}
        self.assertTrue(await self.store.remaining("example"))
        cursor = Cursor([{"_id": 1}, {"_id": 2}])
        self.db.documents.find.return_value = cursor
        with patch("crawler.storage.time.time", return_value=100):
            self.assertEqual(await self.store.due(1, "example"), [{"_id": 1}])
        self.assertEqual(cursor.sort_args, ("next_check_at", 1))
        self.assertEqual(self.db.documents.find.call_args.args[0]["next_check_at"], {"$lte": 100})

    async def test_connect_uses_configured_database(self):
        with patch("crawler.storage.MongoClient") as constructor:
            client, store = connect(config())
        constructor.assert_called_once_with("mongodb://localhost:27017", serverSelectionTimeoutMS=10000)
        self.assertIs(client, constructor.return_value)
        self.assertEqual(store.lease_seconds, 30)
