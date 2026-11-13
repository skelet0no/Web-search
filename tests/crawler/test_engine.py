import hashlib
import io
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

import httpx

from crawler.engine import Engine, quotas
from crawler.http import FetchError, SkipPage
from tests.support import AsyncUnitTestCase, UnitTestCase, config


class QuotaTests(UnitTestCase):
    def test_balanced_allocation_and_remainder(self):
        self.assertEqual(quotas(10, ["a", "b", "c"], {}, set()), {"a": 4, "b": 3, "c": 3})

    def test_exhausted_sources_release_only_unused_quota(self):
        self.assertEqual(quotas(10, ["a", "b", "c"], {"a": 1}, {"a"}), {"a": 1, "b": 5, "c": 4})
        self.assertEqual(quotas(10, ["a", "b"], {"a": 7}, {"a"}), {"a": 7, "b": 5})
        self.assertEqual(quotas(10, ["a", "b"], {"a": 2, "b": 1}, {"a", "b"}), {"a": 2, "b": 1})


class EngineTests(AsyncUnitTestCase):
    def setUp(self):
        super().setUp()
        self.data = config()
        documents = Mock()
        documents.find_one.return_value = None
        documents.update_one.return_value = SimpleNamespace(upserted_id="new")
        documents.count_documents.return_value = 0
        self.store = SimpleNamespace(db=SimpleNamespace(documents=documents, state=Mock(), crawl_queue=Mock()),
                                     owner="owner")
        for name in ("enqueue", "finish", "fail", "setup", "lock", "counts", "remaining", "claim", "due", "release"):
            setattr(self.store, name, AsyncMock())

        async def invoke(function, *args, **kwargs):
            return function(*args, **kwargs)

        self.store.call = AsyncMock(side_effect=invoke)
        self.engine = Engine(self.data, self.store, None)
        self.fetcher = SimpleNamespace(request=AsyncMock(), refresh_robots=AsyncMock(), robots=Mock())
        self.fetcher.robots.site_maps.return_value = []
        self.engine.fetchers["example"] = self.fetcher
        self.job = {"_id": "job", "source": "example", "kind": "page", "url": "https://example.test/a", "attempts": 1}

    def response(self, status=200, raw=b"<p>game</p>", **headers):
        stream = io.BytesIO(raw)
        response = httpx.Response(status, headers={"Content-Type": "text/html; charset=utf-8", **headers},
                                  request=httpx.Request("GET", self.job["url"]))
        self.fetcher.request.return_value = (response, stream)
        return stream

    async def test_bootstrap_filters_external_robot_sitemaps(self):
        self.fetcher.robots.site_maps.return_value = ["https://example.test/other.xml", "https://outside.test/x"]
        await self.engine.bootstrap("example")
        urls = [item[1] for item in self.store.enqueue.call_args.args[1]]
        self.assertEqual(urls, ["https://example.test/sitemap.xml", "https://example.test/other.xml"])

    async def test_bootstrap_fallback_when_robots_unavailable(self):
        self.engine.sources["example"]["sitemap_urls"] = []
        self.fetcher.refresh_robots.side_effect = FetchError("offline")
        await self.engine.bootstrap("example")
        self.store.enqueue.assert_awaited_once_with("example", [("sitemap", "https://example.test/sitemap.xml", None)])

    async def test_new_page_saves_hash_and_increments_count(self):
        stream = self.response(ETag="version")
        self.engine.counts = {"example": 0}
        with patch("crawler.engine.time.time", return_value=100):
            await self.engine.process_page(self.job)
        self.assertTrue(stream.closed)
        fields = self.store.db.documents.update_one.call_args.args[1]["$setOnInsert"]
        self.assertEqual(fields["raw_html"], "<p>game</p>")
        self.assertEqual(fields["content_hash"], hashlib.sha256(b"<p>game</p>").hexdigest())
        self.assertEqual(fields["etag"], "version")
        self.assertEqual(fields["next_check_at"], 100 + 7 * 86400)
        self.assertEqual(self.engine.counts["example"], 1)

    async def test_duplicate_upsert_does_not_increment_count(self):
        self.response()
        self.store.db.documents.update_one.return_value = SimpleNamespace(upserted_id=None)
        self.engine.counts = {"example": 1}
        await self.engine.process_page(self.job)
        self.assertEqual(self.engine.counts["example"], 1)

    async def test_existing_primary_job_does_not_fetch_again(self):
        self.store.db.documents.find_one.return_value = {"_id": "old"}
        await self.engine.process_page(self.job)
        self.fetcher.request.assert_not_awaited()

    async def test_conditional_recrawl_and_304_preserve_html(self):
        existing = {"_id": "old", "etag": "e", "last_modified": "date", "content_hash": "hash"}
        stream = self.response(304)
        await self.engine.process_page({**self.job, "kind": "recrawl"}, existing)
        self.fetcher.request.assert_awaited_once_with(self.job["url"], headers={"If-None-Match": "e", "If-Modified-Since": "date"})
        self.assertNotIn("raw_html", self.store.db.documents.update_one.call_args.args[1]["$set"])
        self.assertTrue(stream.closed)

    async def test_304_without_saved_document_is_error(self):
        stream = self.response(304)
        with self.assertRaises(FetchError):
            await self.engine.process_page(self.job)
        self.assertTrue(stream.closed)

    async def test_unchanged_and_changed_200(self):
        for digest, changed in ((hashlib.sha256(b"<p>game</p>").hexdigest(), False), ("old", True)):
            with self.subTest(changed=changed):
                self.response()
                self.engine.changed = 0
                await self.engine.process_page({**self.job, "kind": "recrawl"}, {"_id": "old", "content_hash": digest})
                fields = self.store.db.documents.update_one.call_args.args[1]["$set"]
                self.assertEqual("raw_html" in fields, changed)
                self.assertEqual(self.engine.changed, int(changed))

    async def test_gone_page_retains_last_version(self):
        for status in (404, 410):
            self.response(status)
            await self.engine.process_page({**self.job, "kind": "recrawl"}, {"_id": "old"})
            fields = self.store.db.documents.update_one.call_args.args[1]["$set"]
            self.assertEqual(fields["last_http_status"], status)
            self.assertNotIn("raw_html", fields)
            self.store.db.documents.delete_one.assert_not_called()

    async def test_non_html_and_decoded_size_rejected(self):
        stream = self.response(**{"Content-Type": "application/pdf"})
        with self.assertRaises(SkipPage):
            await self.engine.process_page(self.job)
        self.assertTrue(stream.closed)
        self.data["logic"]["max_html_bytes"] = 1
        stream = self.response(raw=b"\xff")
        with self.assertRaises(SkipPage):
            await self.engine.process_page(self.job)
        self.assertTrue(stream.closed)

    async def test_sitemap_batches_and_filters_domain(self):
        urls = "".join(f"<url><loc>https://example.test/{i}</loc></url>" for i in range(501))
        xml = f"<urlset>{urls}<url><loc>https://outside.test/a</loc></url></urlset>".encode()
        stream = self.response(raw=xml)
        await self.engine.process_sitemap({**self.job, "kind": "sitemap"})
        self.assertEqual([len(call.args[1]) for call in self.store.enqueue.await_args_list], [500, 1])
        self.assertTrue(stream.closed)

    async def test_process_skip_retry_and_finish_routing(self):
        for kind, error, retries in (("page", SkipPage("skip"), False),
                                      ("sitemap", SkipPage("large"), True),
                                      ("page", FetchError("bad"), True)):
            with self.subTest(kind=kind, error=error):
                self.store.finish.reset_mock()
                self.store.fail.reset_mock()
                self.engine.process_page = AsyncMock(side_effect=error)
                self.engine.process_sitemap = AsyncMock(side_effect=error)
                await self.engine.process({**self.job, "kind": kind})
                self.assertEqual(self.store.fail.await_count, int(retries))
                self.assertEqual(self.store.finish.await_count, int(not retries))
        self.engine.process_page = AsyncMock()
        self.store.finish.reset_mock()
        await self.engine.process(self.job)
        self.store.finish.assert_awaited_once_with(self.job)

    async def test_retry_respects_server_delay_and_caps_exponent(self):
        error = FetchError("busy", retry_after=99)
        await self.engine.retry(self.job, error)
        self.store.fail.assert_awaited_once_with(self.job, error, 99, 3)
        await self.engine.retry({**self.job, "attempts": 100}, ValueError("bad"))
        self.assertEqual(self.store.fail.call_args.args[2], 10 * 2**10)

    async def test_recrawl_failure_is_rescheduled_and_success_resets_errors(self):
        document = {"_id": "old", "source": "example", "url": self.job["url"], "check_attempts": 2}
        self.engine.process_page = AsyncMock(side_effect=FetchError("busy", 90))
        with patch("crawler.engine.time.time", return_value=100):
            await self.engine.recrawl(document)
        fields = self.store.db.documents.update_one.call_args.args[1]["$set"]
        self.assertEqual(fields["check_attempts"], 3)
        self.assertEqual(fields["next_check_at"], 190)
        self.engine.process_page.side_effect = None
        await self.engine.recrawl(document)
        self.assertEqual(self.store.db.documents.update_one.call_args.args[1]["$set"],
                         {"check_attempts": 0, "last_error": None})

    async def test_run_shortfall_and_failed_queue_release_lock(self):
        self.engine.bootstrap = AsyncMock()
        self.store.lock.return_value = True
        self.store.counts.return_value = {"example": 0}
        self.store.claim.return_value = None
        self.store.remaining.return_value = False
        self.assertEqual(await self.engine.run(), "shortfall")
        self.store.release.assert_awaited_once()
        self.assertTrue(self.engine.stop.is_set())
        self.engine.stop.clear()
        self.store.remaining.return_value = True
        self.store.db.crawl_queue.count_documents.side_effect = [2, 0]
        self.assertEqual(await self.engine.run(), "failed")
        self.assertEqual(self.store.release.await_count, 2)

    async def test_run_rejects_second_crawler(self):
        self.store.lock.return_value = False
        with self.assertRaisesRegex(RuntimeError, "Another crawler"):
            await self.engine.run()
        self.store.release.assert_not_awaited()

    async def test_initial_phase_reserves_global_budget_and_waits_for_active_job(self):
        self.data["logic"]["target_documents"] = 1
        self.engine.bootstrap = AsyncMock()
        self.store.lock.return_value = True
        self.store.counts.return_value = {"example": 0}
        self.store.remaining.return_value = True
        self.store.claim.return_value = self.job

        async def complete(job):
            self.engine.counts["example"] += 1
            self.engine.stop.set()

        self.engine.process = AsyncMock(side_effect=complete)
        self.assertEqual(await self.engine.run(), "stopped")
        self.engine.process.assert_awaited_once_with(self.job)
        self.store.claim.assert_awaited_once_with("example", "page")
        self.store.release.assert_awaited_once()

    async def test_recrawl_phase_and_graceful_stop(self):
        self.engine.bootstrap = AsyncMock()
        self.store.lock.return_value = True
        self.store.counts.return_value = {"example": 3}
        self.store.due.return_value = []
        self.engine.pause = AsyncMock(side_effect=self.engine.stop.set)
        self.assertEqual(await self.engine.run(), "stopped")
        self.store.due.assert_awaited_once_with(2, "example")
        self.store.claim.assert_not_awaited()
        self.store.release.assert_awaited_once()

    async def test_heartbeat_lock_loss_stops_crawler(self):
        async def timeout(coroutine, timeout):
            coroutine.close()
            raise TimeoutError

        self.store.lock.return_value = False
        with patch("crawler.engine.asyncio.wait_for", side_effect=timeout):
            with self.assertRaisesRegex(RuntimeError, "Runner lock lost"):
                await self.engine.heartbeat()
        self.assertTrue(self.engine.stop.is_set())

    async def test_heartbeat_renews_only_jobs_owned_by_this_runner(self):
        async def timeout(coroutine, timeout):
            coroutine.close()
            raise TimeoutError

        self.store.lock.return_value = True
        self.store.db.crawl_queue.update_many.side_effect = lambda *a: self.engine.stop.set()
        with patch("crawler.engine.asyncio.wait_for", side_effect=timeout), patch("crawler.engine.time.time", return_value=100):
            await self.engine.heartbeat()
        query, update = self.store.db.crawl_queue.update_many.call_args.args
        self.assertEqual(query, {"status": "running", "owner": "owner"})
        self.assertEqual(update, {"$set": {"lease_until": 130}})
