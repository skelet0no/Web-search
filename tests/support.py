import copy
import socket
import unittest
from unittest.mock import patch


def config():
    return copy.deepcopy({
        "db": {"uri": "mongodb://localhost:27017", "database": "unit_test"},
        "logic": {
            "target_documents": 3, "recrawl_interval_days": 7,
            "request_timeout_seconds": 1, "max_retries": 3,
            "lease_seconds": 30, "max_html_bytes": 8000000,
            "max_sitemap_bytes": 1000000, "poll_seconds": 1,
            "retry_base_seconds": 10, "user_agent": "UnitBot/1.0",
        },
        "sources": [{"name": "example", "domain": "example.test",
                     "sitemap_urls": ["https://example.test/sitemap.xml"],
                     "delay_seconds": 0, "concurrency": 2}],
    })


def block_network(test):
    for target in ("socket.create_connection", "socket.socket.connect", "socket.socket.connect_ex"):
        patcher = patch(target, side_effect=AssertionError("Unit tests must not access the network"))
        patcher.start()
        test.addCleanup(patcher.stop)


class UnitTestCase(unittest.TestCase):
    def setUp(self):
        block_network(self)


class AsyncUnitTestCase(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        block_network(self)


class Cursor:

    def __init__(self, rows):
        self.rows = list(rows)
        self.closed = False
        self.sort_args = self.batch = self.maximum = None

    def __iter__(self):
        return iter(self.rows if self.maximum is None else self.rows[:self.maximum])

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True

    def sort(self, *args):
        self.sort_args = args
        return self

    def batch_size(self, value):
        self.batch = value
        return self

    def limit(self, value):
        self.maximum = value
        return self
