"""Run corpus inspection and extraction with python -m corpus."""

import argparse
import logging
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

from pymongo.errors import PyMongoError

from .config import load_config
from .pipeline import run_analysis
from .repository import Repository


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("Must be positive")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description="Анализ корпуса MongoDB и сохранение отчёта в docx/")
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("inspect", "extract"):
        p = sub.add_parser(command, help="Диагностическая выборка" if command == "inspect" else "Извлечение и полный отчёт")
        p.add_argument("config", help="YAML с db и sources")
        p.add_argument("--uri", help="Переопределить db.uri, например mongodb://127.0.0.1:27017")
        p.add_argument("--run-id", default=None)
        p.add_argument("--source", action="append", dest="sources", help="Повторяется для нескольких источников")
        p.add_argument("--url-pattern", help="Регулярное выражение для отбора URL")
        p.add_argument("--limit", type=positive, default=100 if command == "inspect" else None)
        p.add_argument("--batch-size", type=positive, default=100)
        p.add_argument("--samples-per-source", type=positive, default=5)
        p.add_argument("--output", type=Path, default=Path("docx/runs"))
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        config = load_config(args.config, args.uri)
        run_id = args.run_id or datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid4().hex[:8]
        repository = Repository(config)
        try:
            run_analysis(repository, config, run_id=run_id, output=args.output,
                         sources=args.sources, limit=args.limit, batch_size=args.batch_size,
                         samples_per_source=args.samples_per_source, mode=args.command, url_pattern=args.url_pattern)
        finally:
            repository.close()
        return 0
    except (ValueError, OSError, RuntimeError, PyMongoError) as exc:
        logging.error("%s: %s", type(exc).__name__, exc)
        return 2
    except KeyboardInterrupt:
        logging.error("Analysis interrupted")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
