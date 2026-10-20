import asyncio
import hashlib
import logging
import time
from xml.etree.ElementTree import ParseError

from defusedxml.common import DefusedXmlException

import httpx

from .http import FetchError, Fetcher, SkipPage
from .sitemaps import entries
from .urls import normalize

log = logging.getLogger(__name__)


def quotas(target, sources, counts, exhausted):
    base, extra = divmod(target, len(sources))
    result = {name: base + (i < extra) for i, name in enumerate(sources)}
    spare = sum(max(0, result[name] - counts.get(name, 0)) for name in exhausted)
    for name in exhausted:
        result[name] = counts.get(name, 0)
    active = [name for name in sources if name not in exhausted]
    if active:
        share, remainder = divmod(spare, len(active))
        for i, name in enumerate(active):
            result[name] += share + (i < remainder)
    return result


class Engine:
    def __init__(self, config, store, client):
        self.config, self.store = config, store
        self.logic = config["logic"]
        self.sources = {s["name"]: s for s in config["sources"]}
        self.fetchers = {name: Fetcher(source, self.logic, client) for name, source in self.sources.items()}
        self.stop = asyncio.Event()
        self.interval = self.logic["recrawl_interval_days"] * 86400
        self.started = time.time()
        self.changed = 0
        self.counts = None
        self.last_report = 0

    async def bootstrap(self, name):
        fetcher = self.fetchers[name]
        urls = list(self.sources[name]["sitemap_urls"])
        try:
            await fetcher.refresh_robots()
            urls += fetcher.robots.site_maps() or []
        except (FetchError, SkipPage, httpx.HTTPError, ValueError, OSError) as error:
            log.warning("Robots temporarily unavailable source=%s error=%s", name, error)
        if not urls:
            urls = [f'https://{self.sources[name]["domain"]}/sitemap.xml']
        batch = []
        for url in urls:
            try:
                batch.append(("sitemap", normalize(url, self.sources[name]["domain"]), None))
            except ValueError:
                log.warning("Ignoring external sitemap source=%s url=%s", name, url)
        await self.store.enqueue(name, batch)

    async def process(self, job):
        name = job["source"]
        try:
            if job["kind"] == "sitemap":
                await self.process_sitemap(job)
            else:
                await self.process_page(job)
            await self.store.finish(job)
        except SkipPage as error:
            if job["kind"] == "sitemap":
                await self.retry(job, error)
            else:
                await self.store.finish(job, last_error=str(error))
            log.warning("Skipped source=%s url=%s reason=%s", name, job["url"], error)
        except (FetchError, httpx.HTTPError, ValueError, OSError, ParseError, DefusedXmlException) as error:
            await self.retry(job, error)

    async def retry(self, job, error):
        delay = max(self.logic["retry_base_seconds"] * 2 ** min(job["attempts"], 10), getattr(error, "retry_after", 0))
        await self.store.fail(job, error, delay, self.logic["max_retries"])
        log.warning("Retry url=%s error=%s", job["url"], error)

    async def process_sitemap(self, job):
        response, file = await self.fetchers[job["source"]].request(
            job["url"], max_bytes=self.logic["max_sitemap_bytes"])
        try:
            if response.status_code != 200:
                raise FetchError(f"Sitemap HTTP {response.status_code}")
            batch = []
            for kind, url, lastmod in entries(file, self.logic["max_sitemap_bytes"]):
                try:
                    batch.append((kind, normalize(url, self.sources[job["source"]]["domain"]), lastmod))
                except ValueError:
                    continue
                if len(batch) >= 500:
                    await self.store.enqueue(job["source"], batch)
                    batch = []
                if self.stop.is_set():
                    raise FetchError("Stopped during sitemap processing")
            await self.store.enqueue(job["source"], batch)
        finally:
            file.close()

    async def process_page(self, job, existing=None):
        documents = self.store.db.documents
        existing = existing or await self.store.call(documents.find_one, {"url": job["url"]})
        if existing and job.get("kind") != "recrawl":
            return
        headers = {}
        if existing:
            if existing.get("etag"):
                headers["If-None-Match"] = existing["etag"]
            if existing.get("last_modified"):
                headers["If-Modified-Since"] = existing["last_modified"]
        response, file = await self.fetchers[job["source"]].request(job["url"], headers=headers)
        try:
            now = time.time()
            metadata = dict(last_checked_at=now, next_check_at=now + self.interval,
                            last_http_status=response.status_code)
            if response.status_code == 304:
                if not existing:
                    raise FetchError("Unexpected 304 without saved document")
                await self.store.call(documents.update_one, {"_id": existing["_id"]}, {"$set": metadata})
                return
            if response.status_code in (404, 410):
                if existing:
                    await self.store.call(documents.update_one, {"_id": existing["_id"]}, {"$set": metadata})
                return
            if response.status_code != 200:
                raise FetchError(f"Page HTTP {response.status_code}")
            if response.headers.get("content-type", "").split(";", 1)[0].lower() not in ("text/html", "application/xhtml+xml"):
                raise SkipPage("Not HTML")
            raw = file.read()
            html = raw.decode(response.encoding or "utf-8", errors="replace")
            if len(html.encode("utf-8")) > self.logic["max_html_bytes"]:
                raise SkipPage("Decoded HTML exceeds size limit")
            digest = hashlib.sha256(raw).hexdigest()
            metadata.update(etag=response.headers.get("etag"), last_modified=response.headers.get("last-modified"))
            if not existing or existing["content_hash"] != digest:
                metadata.update(raw_html=html, content_hash=digest, crawled_at=int(now))
                self.changed += bool(existing)
            if existing:
                await self.store.call(documents.update_one, {"_id": existing["_id"]}, {"$set": metadata})
            else:
                url = normalize(str(response.url), self.sources[job["source"]]["domain"])
                result = await self.store.call(documents.update_one, {"url": url},
                                      {"$setOnInsert": dict(url=url, source=job["source"], **metadata)}, upsert=True)
                if result.upserted_id is not None and self.counts is not None:
                    self.counts[job["source"]] = self.counts.get(job["source"], 0) + 1
        finally:
            file.close()

    async def heartbeat(self):
        while not self.stop.is_set():
            try:
                await asyncio.wait_for(self.stop.wait(), timeout=self.logic["lease_seconds"] / 3)
            except asyncio.TimeoutError:
                if not await self.store.lock():
                    self.stop.set()
                    raise RuntimeError("Runner lock lost")
                await self.store.call(self.store.db.crawl_queue.update_many,
                    {"status": "running", "owner": self.store.owner},
                    {"$set": {"lease_until": time.time() + self.logic["lease_seconds"]}})

    async def pause(self):
        try:
            await asyncio.wait_for(self.stop.wait(), timeout=self.logic["poll_seconds"])
        except asyncio.TimeoutError:
            pass

    async def recrawl(self, document):
        job = dict(url=document["url"], source=document["source"], kind="recrawl")
        try:
            await self.process_page(job, document)
        except (SkipPage, FetchError, httpx.HTTPError, ValueError, OSError) as error:
            attempts = document.get("check_attempts", 0) + 1
            delay = max(self.logic["retry_base_seconds"] * 2 ** min(attempts, 10), getattr(error, "retry_after", 0))
            await self.store.call(self.store.db.documents.update_one, {"_id": document["_id"]},
                {"$set": dict(next_check_at=time.time() + delay, check_attempts=attempts, last_error=str(error))})
            log.warning("Recrawl failed url=%s error=%s", document["url"], error)
        else:
            await self.store.call(self.store.db.documents.update_one, {"_id": document["_id"]},
                                  {"$set": {"check_attempts": 0, "last_error": None}})

    async def run(self):
        await self.store.setup()
        if not await self.store.lock():
            raise RuntimeError("Another crawler holds this database; wait for its lease to expire")
        heartbeat = asyncio.create_task(self.heartbeat())
        active = {}
        try:
            self.counts = await self.store.counts()
            initial_total = sum(self.counts.values())
            await asyncio.gather(*(self.bootstrap(name) for name in self.sources))
            while not self.stop.is_set():
                for task in list(active):
                    if task.done():
                        del active[task]
                        task.result()
                if heartbeat.done():
                    heartbeat.result()
                counts = self.counts
                total = sum(counts.values())
                phase = "recrawl" if total >= self.logic["target_documents"] else "initial"
                await self.store.call(self.store.db.state.update_one, {"_id": "progress"},
                    {"$set": dict(phase=phase, counts=counts, updated_at=time.time())}, upsert=True)
                if time.time() - self.last_report >= 30:
                    overdue = await self.store.call(self.store.db.documents.count_documents,
                        {"next_check_at": {"$lte": time.time()}})
                    rate = (total - initial_total) / max(1, time.time() - self.started)
                    log.info("phase=%s total=%s target=%s counts=%s changed=%s overdue=%s docs_per_second=%.3f", phase, total,
                             self.logic["target_documents"], counts, self.changed, overdue, rate)
                    self.last_report = time.time()
                if phase == "recrawl":
                    if active:
                        await asyncio.wait(active, return_when=asyncio.FIRST_COMPLETED)
                        continue
                    due = []
                    for name, source in self.sources.items():
                        due.extend(await self.store.due(source["concurrency"], name))
                    if due:
                        await asyncio.gather(*(self.recrawl(d) for d in due))
                    else:
                        await self.pause()
                    continue
                exhausted = {name for name in self.sources if not await self.store.remaining(name)}
                limits = quotas(self.logic["target_documents"], list(self.sources), counts, exhausted)
                for name, source in self.sources.items():
                    source_jobs = [job for job in active.values() if job["source"] == name]
                    reserved = sum(job["kind"] == "page" for job in source_jobs)
                    global_reserved = sum(job["kind"] == "page" for job in active.values())
                    budget = self.logic["target_documents"] - sum(counts.values()) - global_reserved
                    slots = min(source["concurrency"] - len(source_jobs),
                                max(0, limits[name] - counts.get(name, 0) - reserved), budget)
                    for _ in range(slots):
                        job = await self.store.claim(name, "page")
                        if not job:
                            if not any(j["source"] == name and j["kind"] == "sitemap" for j in active.values()):
                                job = await self.store.claim(name, "sitemap")
                            if job:
                                active[asyncio.create_task(self.process(job))] = job
                            break
                        active[asyncio.create_task(self.process(job))] = job
                if active:
                    await asyncio.wait(active, return_when=asyncio.FIRST_COMPLETED)
                elif len(exhausted) == len(self.sources):
                    log.error("All sitemaps exhausted: collected=%s target=%s", total, self.logic["target_documents"])
                    return "shortfall"
                else:
                    failed = await self.store.call(self.store.db.crawl_queue.count_documents, {"status": "failed"})
                    retryable = await self.store.call(self.store.db.crawl_queue.count_documents,
                        {"status": {"$in": ["pending", "running"]}, "source": {"$in": [
                            name for name in self.sources if counts.get(name, 0) < limits[name]]}})
                    if failed and not retryable:
                        log.error("No ready work; %s failed tasks retained. See README to retry.", failed)
                        return "failed"
                    await self.pause()
            return "stopped"
        finally:
            self.stop.set()
            if active:
                await asyncio.gather(*active, return_exceptions=True)
            heartbeat.cancel()
            await asyncio.gather(heartbeat, return_exceptions=True)
            await self.store.release()
