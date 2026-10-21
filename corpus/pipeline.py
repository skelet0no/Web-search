"""Streaming corpus analysis and persistent run lifecycle."""

import hashlib
import logging
import re
from contextlib import nullcontext
from datetime import datetime, timezone
from pathlib import Path

from .extract import EXTRACTOR_VERSION, extract_document
from .report import Samples, write_report
from .stats import Statistics

log = logging.getLogger(__name__)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def run_analysis(repository, config, *, run_id, output, sources=None, limit=None,
                 batch_size=100, samples_per_source=5, mode="extract", url_pattern=None):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}", run_id):
        raise ValueError("run_id: use 1–80 ASCII letters, digits, '.', '_' or '-' starting with a letter/digit")
    if limit is not None and limit < 1:
        raise ValueError("limit must be positive")
    if batch_size < 1 or samples_per_source < 2:
        raise ValueError("batch_size must be positive; samples_per_source must be at least 2")
    if mode not in {"inspect", "extract"}:
        raise ValueError("Unknown analysis mode")
    preview = mode == "inspect"
    if preview and limit is None:
        limit = 100
    known_sources = [s["name"] for s in config["sources"]]
    sources = sorted(set(sources or known_sources))
    if known_sources and any(s not in known_sources for s in sources):
        raise ValueError("Unknown source (see sources in config)")
    base = {"source": {"$in": sources}} if sources else {}
    if url_pattern:
        try:
            re.compile(url_pattern)
        except re.error as exc:
            raise ValueError(f"Invalid URL pattern: {exc}") from exc
        base["url"] = {"$regex": url_pattern}
    query = {**base, "raw_html": {"$type": "string", "$regex": r"\S"},
             "$expr": {"$eq": [{"$type": "$raw_html"}, "string"]}}
    directory = Path(output) / run_id
    if directory.exists():
        raise ValueError(f"Output already exists: {directory}. Use a new run_id.")
    summary = dict(run_id=run_id, mode=mode, state="running", started_at=utc_now(),
                   extractor_version=EXTRACTOR_VERSION, is_sample=limit is not None,
                   snapshot_mode="live_preview" if preview else "crawler_lease",
                   selection=dict(database=config["db"]["database"], collection="documents",
                                  sources=sources, limit=limit, url_pattern=url_pattern,
                                  order="_id ascending", filter=query))
    stats, samples = Statistics(), Samples(samples_per_source)
    def update_run(**values):
        if not preview:
            repository.update_run(run_id, **values)

    def fail_run(exc):
        summary.update(state="failed", finished_at=utc_now(), processed_documents=stats.total.count,
                       error=type(exc).__name__, statistics=stats.as_dict())
        # Do not leave a report claiming completion after a failed run.
        if directory.exists():
            try:
                (directory / "report.md").unlink(missing_ok=True)
            except OSError:
                log.error("Cannot persist local failure state for %s", run_id)
        try:
            update_run(**{k: v for k, v in summary.items() if k != "run_id"})
        except Exception as recording_error:
            log.error("Cannot persist MongoDB failure state: %s", type(recording_error).__name__)

    # Preview is read-only and explicitly does not promise a fixed corpus snapshot.
    with (nullcontext(lambda: None) if preview else repository.frozen()) as check:
        if not preview:
            repository.setup()
        summary["available_documents"] = repository.db.documents.count_documents(base)
        summary["eligible_documents"] = repository.db.documents.count_documents(query)
        summary["excluded_documents"] = summary["available_documents"] - summary["eligible_documents"]
        if not preview:
            repository.create_run(summary)
        try:
            directory.mkdir(parents=True, exist_ok=False)
            with repository.documents(query, limit, batch_size) as cursor:
                for original in cursor:
                    if stats.total.count % batch_size == 0:
                        check()
                    raw = original["raw_html"]
                    try:
                        result = extract_document(raw, original.get("source"))
                    except Exception as exc:
                        log.warning("Extraction failed for %s: %s", original["_id"], type(exc).__name__)
                        result = dict(title=None, text="", status="error", raw_utf8_bytes=len(raw.encode("utf-8")),
                                      text_utf8_bytes=0, warnings=[f"extraction_error:{type(exc).__name__}"], features={})
                    record = dict(result, run_id=run_id, document_id=original["_id"],
                                  url=original.get("url", ""), source=original.get("source") or "unknown",
                                  source_content_hash=original.get("content_hash"),
                                  saved_html_sha256=hashlib.sha256(raw.encode("utf-8")).hexdigest(),
                                  extractor_version=EXTRACTOR_VERSION, extracted_at=utc_now(),
                                  last_http_status=original.get("last_http_status"))
                    if not preview:
                        repository.save_document(record)
                    stats.add(original, record)
                    samples.add(original, record)
                    if stats.total.count % batch_size == 0:
                        log.info("Processed %s documents", stats.total.count)
            expected = min(summary["eligible_documents"], limit) if limit else summary["eligible_documents"]
            if not preview and stats.total.count != expected:
                raise RuntimeError("Document count changed during analysis; snapshot is incomplete")
            check()
            summary.update(statistics=stats.as_dict(), samples=samples.export(directory),
                           processed_documents=stats.total.count)
        except BaseException as exc:
            fail_run(exc)
            raise
        # Export before reporting completion: filesystem failures must fail the run.
        try:
            check()
            summary.update(state="completed", finished_at=utc_now())
            write_report(directory, summary)
            check()
            update_run(**{k: v for k, v in summary.items() if k != "run_id"})
        except BaseException as exc:
            fail_run(exc)
            raise
    log.info("Completed %s: %s documents; %s", run_id, stats.total.count, directory / "report.md")
    return summary
