from pathlib import Path
import math
from urllib.parse import urlsplit

import yaml


def load_config(path: str) -> dict:
    with Path(path).open(encoding="utf-8") as stream:
        config = yaml.safe_load(stream)
    if not isinstance(config, dict):
        raise ValueError("Configuration must be a mapping")
    db = config.get("db", {})
    if not isinstance(db, dict):
        raise ValueError("db must be a mapping")
    if not isinstance(db.get("uri"), str) or not db["uri"].startswith(("mongodb://", "mongodb+srv://")):
        raise ValueError("db.uri must be a MongoDB URI")
    if not isinstance(db.get("database"), str) or not db["database"]:
        raise ValueError("db.database is required")
    logic = config.setdefault("logic", {})
    if not isinstance(logic, dict):
        raise ValueError("logic must be a mapping")
    defaults = dict(target_documents=1000, recrawl_interval_days=7,
                    request_timeout_seconds=30, max_retries=5,
                    lease_seconds=300, max_html_bytes=8_000_000,
                    max_sitemap_bytes=100_000_000, poll_seconds=5,
                    retry_base_seconds=30, user_agent="SitemapCrawler/0.1")
    for key, default in defaults.items():
        logic.setdefault(key, default)
        if isinstance(default, (int, float)):
            value = logic[key]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                raise ValueError(f"logic.{key} must be positive")
    for key in ("target_documents", "max_retries", "max_html_bytes", "max_sitemap_bytes"):
        if not isinstance(logic[key], int):
            raise ValueError(f"logic.{key} must be an integer")
    if logic["max_html_bytes"] > 8_000_000:
        raise ValueError("max_html_bytes must not exceed 8000000 (MongoDB BSON limit)")
    if not isinstance(logic["user_agent"], str) or not logic["user_agent"].strip():
        raise ValueError("user_agent must be nonempty")
    sources = config.get("sources")
    if not isinstance(sources, list) or not sources:
        raise ValueError("sources must be a nonempty list")
    names, domains = set(), set()
    for source in sources:
        if not isinstance(source, dict) or not isinstance(source.get("name"), str) or not isinstance(source.get("domain"), str):
            raise ValueError("Each source needs a string name and domain")
        name, domain = source.get("name"), source.get("domain", "").lower()
        if not name or name in names or not domain or domain in domains or urlsplit("https://" + domain).netloc != domain or "/" in domain:
            raise ValueError("Sources need unique names and plain unique domains")
        names.add(name)
        domains.add(domain)
        source["domain"] = domain
        source.setdefault("sitemap_urls", [])
        source.setdefault("delay_seconds", 2)
        source.setdefault("concurrency", 2)
        if isinstance(source["concurrency"], bool) or not isinstance(source["concurrency"], int) or source["concurrency"] < 1:
            raise ValueError("concurrency must be a positive integer")
        if not isinstance(source["delay_seconds"], (int, float)) or isinstance(source["delay_seconds"], bool) or not math.isfinite(source["delay_seconds"]) or source["delay_seconds"] < 0:
            raise ValueError("delay_seconds must be nonnegative")
        if not isinstance(source["sitemap_urls"], list):
            raise ValueError("sitemap_urls must be a list")
        for url in source["sitemap_urls"]:
            if not isinstance(url, str) or urlsplit(url).scheme not in ("http", "https") or urlsplit(url).hostname != domain:
                raise ValueError("Sitemap URLs must belong to their source domain")
    return config
