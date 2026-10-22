import threading
import time
from collections import deque
from contextlib import ExitStack, contextmanager
from itertools import islice
from uuid import uuid4

from pymongo import MongoClient, ReturnDocument
from pymongo.errors import DuplicateKeyError


class Repository:
    def __init__(self, config):
        self.client = MongoClient(config["db"]["uri"], serverSelectionTimeoutMS=10000)
        self.db = self.client[config["db"]["database"]]

    def close(self):
        self.client.close()

    def setup(self):
        self.db.corpus_documents.create_index([("run_id", 1), ("document_id", 1)], unique=True)

    @contextmanager
    def frozen(self):
        owner = "corpus-" + str(uuid4())
        stop = threading.Event()
        lost = threading.Event()
        try:
            lock = self.db.state.find_one_and_update(
                {"_id": "runner", "until": {"$lt": time.time()}},
                {"$set": {"owner": owner, "until": time.time() + 300}},
                upsert=True, return_document=ReturnDocument.AFTER)
        except DuplicateKeyError:
            raise ValueError("Crawler or another analysis is active. Stop crawler and wait for its lease to expire.") from None
        if not lock:
            raise ValueError("Cannot acquire corpus lock")

        def renew():
            while not stop.wait(30):
                try:
                    updated = self.db.state.update_one(
                        {"_id": "runner", "owner": owner, "until": {"$gt": time.time()}},
                        {"$set": {"until": time.time() + 300}})
                    if updated.matched_count != 1:
                        lost.set()
                        return
                except Exception:
                    lost.set()
                    return

        def check():
            if lost.is_set():
                raise RuntimeError("Corpus lease lost; run cannot be considered a fixed snapshot")
            current = self.db.state.find_one({"_id": "runner", "owner": owner, "until": {"$gt": time.time()}}, {"_id": 1})
            if current is None:
                raise RuntimeError("Corpus lease expired")

        thread = threading.Thread(target=renew, daemon=True)
        thread.start()
        try:
            yield check
        finally:
            stop.set()
            thread.join(timeout=15)
            self.db.state.delete_one({"_id": "runner", "owner": owner})

    def documents(self, query, limit, batch_size):
        fields = {key: 1 for key in ("url", "source", "raw_html", "content_hash")}
        cursor = self.db.documents.find(query, fields).sort("_id", 1).batch_size(batch_size)
        if limit:
            cursor = cursor.limit(limit)
        return cursor

    @contextmanager
    def sample_documents(self, query, sources, limit, batch_size):
        with ExitStack() as stack:
            cursors = deque(iter(stack.enter_context(self.documents(
                {**query, "source": source}, limit, batch_size))) for source in sources)

            def interleave():
                while cursors:
                    cursor = cursors.popleft()
                    try:
                        document = next(cursor)
                    except StopIteration:
                        continue
                    cursors.append(cursor)
                    yield document

            yield islice(interleave(), limit)

    def create_run(self, run):
        try:
            self.db.corpus_runs.insert_one(dict(run, _id=run["run_id"]))
        except DuplicateKeyError:
            raise ValueError("run_id already exists; use another ID. Existing runs are immutable.") from None

    def save_document(self, record):
        self.db.corpus_documents.insert_one(record)

    def update_run(self, run):
        self.db.corpus_runs.update_one({"_id": run["run_id"]}, {"$set": run})
