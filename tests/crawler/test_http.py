import io
from unittest.mock import AsyncMock, patch

import httpx

from crawler.http import FetchError, Fetcher, SkipPage, make_client, retry_seconds
from tests.support import AsyncUnitTestCase, UnitTestCase, config


class RetryTests(UnitTestCase):
    def test_retry_after_seconds_date_and_invalid_input(self):
        for value, expected in ((None, 0), ("", 0), ("12", 12), ("-1", 0), ("bad", 0)):
            with self.subTest(value=value):
                self.assertEqual(retry_seconds(value), expected)
        with patch("crawler.http.time.time", return_value=0):
            self.assertEqual(retry_seconds("Thu, 01 Jan 1970 00:01:00 GMT"), 60)

    def test_client_configuration(self):
        with patch("crawler.http.httpx.AsyncClient") as client:
            make_client(config()["logic"])
        self.assertEqual(client.call_args.kwargs["headers"], {"User-Agent": "UnitBot/1.0"})
        self.assertFalse(client.call_args.kwargs["follow_redirects"])


class FetcherTests(AsyncUnitTestCase):
    def fetcher(self, client):
        data = config()
        fetcher = Fetcher(data["sources"][0], data["logic"], client)
        fetcher.wait = AsyncMock()
        return fetcher

    async def test_robots_loaded_once_and_response_rewound(self):
        requests = []

        def handle(request):
            requests.append(request.url.path)
            return httpx.Response(200, text="User-agent: *\nAllow: /" if request.url.path == "/robots.txt" else "body")

        async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
            fetcher = self.fetcher(client)
            for _ in range(2):
                response, stream = await fetcher.request("https://example.test/page")
                with stream:
                    self.assertEqual(response.status_code, 200)
                    self.assertEqual(stream.read(), b"body")
        self.assertEqual(requests, ["/robots.txt", "/page", "/page"])

    async def test_disallowed_page_is_not_requested(self):
        paths = []

        def handle(request):
            paths.append(request.url.path)
            return httpx.Response(200, text="User-agent: *\nDisallow: /")

        async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
            with self.assertRaises(SkipPage):
                await self.fetcher(client).request("https://example.test/private")
        self.assertEqual(paths, ["/robots.txt"])

    async def test_robot_http_status_policy(self):
        for status, allowed in ((401, False), (403, False), (404, True), (410, True)):
            with self.subTest(status=status):
                async with httpx.AsyncClient(transport=httpx.MockTransport(lambda r: httpx.Response(status))) as client:
                    fetcher = self.fetcher(client)
                    await fetcher.refresh_robots()
                    self.assertEqual(fetcher.robots.can_fetch("UnitBot", "https://example.test/a"), allowed)

    async def test_robots_delay_and_rate(self):
        body = "User-agent: *\nCrawl-delay: 3\nRequest-rate: 1/5"
        async with httpx.AsyncClient(transport=httpx.MockTransport(lambda r: httpx.Response(200, text=body))) as client:
            fetcher = self.fetcher(client)
            await fetcher.refresh_robots()
            self.assertEqual(fetcher.delay, 5)

    async def test_same_domain_redirect_drops_conditional_headers(self):
        seen = []

        def handle(request):
            seen.append(request)
            return httpx.Response(302, headers={"Location": "/final"}) if request.url.path == "/start" else httpx.Response(200, content=b"ok")

        async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
            response, stream = await self.fetcher(client).request("https://example.test/start",
                                                                headers={"If-None-Match": "old"}, robots=False)
            with stream:
                self.assertEqual(stream.read(), b"ok")
        self.assertEqual(response.url.path, "/final")
        self.assertEqual(seen[0].headers["If-None-Match"], "old")
        self.assertNotIn("If-None-Match", seen[1].headers)

    async def test_redirect_boundaries_and_missing_location(self):
        for location in ("https://outside.test/x", None):
            with self.subTest(location=location):
                seen = []

                def handle(request):
                    seen.append(request)
                    return httpx.Response(302, headers={"Location": location} if location else {})

                async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
                    with self.assertRaises((ValueError, FetchError)):
                        await self.fetcher(client).request("https://example.test/start", robots=False)
                self.assertEqual(len(seen), 1)

    async def test_redirect_loop_is_bounded(self):
        handler = httpx.MockTransport(lambda r: httpx.Response(302, headers={"Location": "/loop"}))
        async with httpx.AsyncClient(transport=handler) as client:
            fetcher = self.fetcher(client)
            with self.assertRaisesRegex(FetchError, "Too many redirects"):
                await fetcher.request("https://example.test/loop", robots=False)
            self.assertEqual(fetcher.wait.await_count, 11)

    async def test_throttle_errors_set_cooldown(self):
        for status in (429, 500, 503):
            with self.subTest(status=status):
                async with httpx.AsyncClient(transport=httpx.MockTransport(
                        lambda r: httpx.Response(status, headers={"Retry-After": "20"}))) as client:
                    fetcher = self.fetcher(client)
                    with patch("crawler.http.time.monotonic", return_value=100):
                        with self.assertRaises(FetchError) as raised:
                            await fetcher.request("https://example.test/a", robots=False)
                    self.assertEqual(raised.exception.retry_after, 20)
                    self.assertEqual(fetcher.next_start, 120)

    async def test_response_limit_closes_temporary_file(self):
        stream = io.BytesIO()
        async with httpx.AsyncClient(transport=httpx.MockTransport(lambda r: httpx.Response(200, content=b"12345"))) as client:
            with patch("crawler.http.tempfile.SpooledTemporaryFile", return_value=stream):
                with self.assertRaises(SkipPage):
                    await self.fetcher(client).request("https://example.test/a", robots=False, max_bytes=4)
        self.assertTrue(stream.closed)

    async def test_scheduler_wait_uses_monotonic_time(self):
        fetcher = Fetcher(config()["sources"][0], config()["logic"], None)
        fetcher.delay, fetcher.next_start = 3, 102
        with patch("crawler.http.time.monotonic", return_value=100), patch("crawler.http.asyncio.sleep", new_callable=AsyncMock) as sleep:
            await fetcher.wait()
        sleep.assert_awaited_once_with(2)
        self.assertEqual(fetcher.next_start, 103)
