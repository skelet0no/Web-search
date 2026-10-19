import asyncio
import time
from uuid import uuid4

from pymongo import MongoClient, ReturnDocument, UpdateOne
from pymongo.errors import DuplicateKeyError


class Store:
    """Synchronous MongoDB operations run off the asyncio event loop."""

    def __init__(self, db, lease_seconds=300):
        self.db = db
        self.lease_seconds = lease_seconds
        self.owner = str(uuid4())

    async def call(self, function, *args, **kwargs):
        return await asyncio.to_thread(function, *args, **kwargs)

    async def setup(self):
        for collection in (self.db.documents, self.db.crawl_queue):
            await self.call(collection.create_index, "url", unique=True)
        await self.call(self.db.crawl_queue.create_index, [("source", 1), ("kind", 1), ("status", 1), ("next_attempt", 1)])
        await self.call(self.db.documents.create_index, "next_check_at")
        await self.call(self.db.documents.create_index, [("source", 1), ("next_check_at", 1)])

    async def lock(self):
        now = time.time()
        try:
            result = await self.call(self.db.state.find_one_and_update,
                {"_id": "runner", "$or": [{"until": {"$lt": now}}, {"owner": self.owner}]},
                {"$set": {"owner": self.owner, "until": now + self.lease_seconds}},
                upsert=True, return_document=ReturnDocument.AFTER)
            return result is not None
        except DuplicateKeyError:
            return False

    async def release(self):
        await self.call(self.db.state.delete_one, {"_id": "runner", "owner": self.owner})

    async def enqueue(self, source, batch):
        operations = [UpdateOne({"url": url}, {"$setOnInsert": dict(
            url=url, source=source, kind=kind, lastmod=lastmod,
            status="pending", attempts=0, next_attempt=0)}, upsert=True)
            for kind, url, lastmod in batch]
        if operations:
            await self.call(self.db.crawl_queue.bulk_write, operations, ordered=False)

    async def claim(self, source, kind):
        now = time.time()
        return await self.call(self.db.crawl_queue.find_one_and_update,
            {"source": source, "kind": kind, "$or": [
                {"status": "pending", "next_attempt": {"$lte": now}},
                {"status": "running", "lease_until": {"$lte": now}}]},
            {"$set": {"status": "running", "lease_until": now + self.lease_seconds,
                       "owner": self.owner}}, sort=[("next_attempt", 1)],
            return_document=ReturnDocument.AFTER)

    async def finish(self, job, **fields):
        await self.call(self.db.crawl_queue.update_one,
            {"_id": job["_id"], "owner": self.owner},
            {"$set": {"status": "done", **fields}})

    async def fail(self, job, error, delay, max_retries):
        attempts = job["attempts"] + 1
        # Transient exhaustion is retained as 'failed', never mistaken for no URLs.
        await self.call(self.db.crawl_queue.update_one,
            {"_id": job["_id"], "owner": self.owner}, {"$set": dict(
                status="failed" if attempts >= max_retries else "pending",
                attempts=attempts, last_error=str(error), next_attempt=time.time() + delay)})

    async def counts(self):
        rows = await self.call(lambda: list(self.db.documents.aggregate([
            {"$group": {"_id": "$source", "count": {"$sum": 1}}}])))
        return {row["_id"]: row["count"] for row in rows}

    async def remaining(self, source):
        return await self.call(self.db.crawl_queue.find_one,
                               {"source": source, "status": {"$ne": "done"}}, {"_id": 1})

    async def due(self, limit, source):
        return await self.call(lambda: list(self.db.documents.find(
            {"source": source, "next_check_at": {"$lte": time.time()}}).sort("next_check_at", 1).limit(limit)))


def connect(config):
    client = MongoClient(config["db"]["uri"], serverSelectionTimeoutMS=10000)
    return client, Store(client[config["db"]["database"]], config["logic"]["lease_seconds"])
