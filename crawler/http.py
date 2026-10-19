import asyncio
import time
import tempfile
from email.utils import parsedate_to_datetime
from urllib.parse import urljoin

import httpx

from .urls import normalize
from .robots import RobotsRules


class FetchError(Exception):
    def __init__(self, message, retry_after=0):
        super().__init__(message)
        self.retry_after = retry_after


class SkipPage(Exception):
    pass


def retry_seconds(value):
    if not value:
        return 0
    try:
        return max(0, float(value))
    except ValueError:
        try:
            return max(0, parsedate_to_datetime(value).timestamp() - time.time())
        except (ValueError, TypeError, OverflowError):
            return 0


class Fetcher:
    def __init__(self, source, logic, client):
        self.source, self.logic, self.client = source, logic, client
        self.robots = None
        self.robots_at = 0
        self.delay = source["delay_seconds"]
        self.next_start = 0
        self.gate = asyncio.Lock()
        self.robots_lock = asyncio.Lock()
        self.semaphore = asyncio.Semaphore(source["concurrency"])

    async def wait(self):
        async with self.gate:
            await asyncio.sleep(max(0, self.next_start - time.monotonic()))
            self.next_start = time.monotonic() + self.delay

    async def refresh_robots(self):
        async with self.robots_lock:
            if self.robots is not None and time.time() - self.robots_at < 86400:
                return
            url = f'https://{self.source["domain"]}/robots.txt'
            response, file = await self.request(url, robots=False, max_bytes=1_000_000)
            try:
                if response.status_code in (401, 403):
                    lines = ["User-agent: *", "Disallow: /"]
                elif response.status_code in (404, 410):
                    lines = []
                elif response.status_code == 200:
                    lines = file.read().decode("utf-8", errors="replace").splitlines()
                else:
                    raise FetchError(f"robots.txt HTTP {response.status_code}")
                parser = RobotsRules(url)
                parser.parse(lines)
                agent = self.logic["user_agent"]
                delay = parser.crawl_delay(agent) or 0
                rate = parser.request_rate(agent)
                self.delay = max(self.source["delay_seconds"], delay,
                                 rate.seconds / rate.requests if rate else 0)
                self.robots, self.robots_at = parser, time.time()
            finally:
                file.close()

    async def request(self, url, headers=None, robots=True, max_bytes=None):
        if robots:
            await self.refresh_robots()
        async with self.semaphore:
            for _ in range(11):
                url = normalize(url, self.source["domain"])
                if robots and not self.robots.can_fetch(self.logic["user_agent"], url):
                    raise SkipPage("Disallowed by robots.txt")
                await self.wait()
                async with self.client.stream("GET", url, headers=headers) as response:
                    if response.status_code in (301, 302, 303, 307, 308):
                        if not response.headers.get("location"):
                            raise FetchError("Redirect without Location")
                        url = urljoin(url, response.headers["location"])
                        # Validators belong to the original representation.
                        headers = None
                        continue
                    if response.status_code == 429 or response.status_code >= 500:
                        cooldown = max(self.delay * 2, retry_seconds(response.headers.get("retry-after")))
                        self.next_start = max(self.next_start, time.monotonic() + cooldown)
                        raise FetchError(f"HTTP {response.status_code}", cooldown)
                    file = tempfile.SpooledTemporaryFile(max_size=1_000_000)
                    try:
                        size = 0
                        async for chunk in response.aiter_bytes():
                            size += len(chunk)
                            if size > (max_bytes or self.logic["max_html_bytes"]):
                                raise SkipPage("Response exceeds size limit")
                            file.write(chunk)
                        file.seek(0)
                        return response, file
                    except BaseException:
                        file.close()
                        raise
            raise FetchError("Too many redirects")


def make_client(logic):
    return httpx.AsyncClient(timeout=logic["request_timeout_seconds"],
                             headers={"User-Agent": logic["user_agent"]},
                             follow_redirects=False)
