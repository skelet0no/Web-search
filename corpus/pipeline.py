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
    balanced = limit is not None and len(sources) > 1
    query = {"raw_html": {"$regex": r"\S"},
             "$expr": {"$eq": [{"$type": "$raw_html"}, "string"]}}
    if sources:
        query["source"] = {"$in": sources}
    if url_pattern:
        try:
            re.compile(url_pattern)
        except re.error as exc:
            raise ValueError(f"Invalid URL pattern: {exc}") from exc
        query["url"] = {"$regex": url_pattern}
    directory = Path(output) / run_id
    if directory.exists():
        raise ValueError(f"Output already exists: {directory}. Use a new run_id.")
    summary = dict(run_id=run_id, mode=mode, state="running", started_at=utc_now(),
                   extractor_version=EXTRACTOR_VERSION, is_sample=limit is not None,
                   snapshot_mode="live_preview" if preview else "crawler_lease",
                   selection=dict(database=config["db"]["database"], collection="documents",
                                  sources=sources, limit=limit, url_pattern=url_pattern,
                                  order="round-robin by source, _id ascending within source" if balanced else "_id ascending",
                                  filter=query))
    stats, samples = Statistics(), Samples(samples_per_source)
    with (nullcontext(lambda: None) if preview else repository.frozen()) as check:
        if not preview:
            repository.setup()
            expected = repository.db.documents.count_documents(query)
            if limit:
                expected = min(expected, limit)
            repository.create_run(summary)
        try:
            directory.mkdir(parents=True, exist_ok=False)
            documents = repository.sample_documents(query, sources, limit, batch_size) if balanced else repository.documents(query, limit, batch_size)
            with documents as cursor:
                for original in cursor:
                    if stats.total.count % batch_size == 0:
                        check()
                    raw = original["raw_html"]
                    try:
                        result = extract_document(raw, original.get("source"))
                    except Exception as exc:
                        log.warning("Extraction failed for %s: %s", original["_id"], type(exc).__name__)
                        result = dict(title=None, text="", status="error", page_type="unknown", raw_utf8_bytes=len(raw.encode("utf-8")),
                                      text_utf8_bytes=0, warnings=[f"extraction_error:{type(exc).__name__}"], features={})
                    record = dict(result, run_id=run_id, document_id=original["_id"],
                                  url=original.get("url", ""), source=original.get("source") or "unknown",
                                  source_content_hash=original.get("content_hash"),
                                  extractor_version=EXTRACTOR_VERSION, extracted_at=utc_now())
                    if not preview:
                        repository.save_document(record)
                    stats.add(record)
                    samples.add(original, record)
                    if stats.total.count % batch_size == 0:
                        log.info("Processed %s documents", stats.total.count)
            if not preview and stats.total.count != expected:
                raise RuntimeError("Document count changed during analysis; snapshot is incomplete")
            if sources and (limit is None or limit >= len(sources)):
                for source in sources:
                    if source not in stats.sources:
                        log.warning("No documents matching the selection for source=%s", source)
            check()
            summary.update(statistics=stats.as_dict(), samples=samples.export(directory),
                           processed_documents=stats.total.count)
            summary.update(state="completed", finished_at=utc_now())
            write_report(directory, summary)
            check()
            if not preview:
                repository.update_run(summary)
        except BaseException as exc:
            summary.update(state="failed", finished_at=utc_now(), processed_documents=stats.total.count,
                           error=type(exc).__name__, statistics=stats.as_dict())
            try:
                (directory / "report.md").unlink(missing_ok=True)
            except OSError:
                log.error("Cannot remove incomplete report for %s", run_id)
            if not preview:
                try:
                    repository.update_run(summary)
                except Exception as recording_error:
                    log.error("Cannot persist failure state: %s", type(recording_error).__name__)
            raise
    log.info("Completed %s: %s documents; %s", run_id, stats.total.count, directory / "report.md")
    return summary
