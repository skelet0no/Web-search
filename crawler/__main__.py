import argparse
import asyncio
import logging
import signal

from .config import load_config
from .engine import Engine
from .http import make_client
from .storage import connect


async def run(config):
    client, store = connect(config)
    try:
        async with make_client(config["logic"]) as http:
            engine = Engine(config, store, http)
            loop = asyncio.get_running_loop()
            for signum in (signal.SIGINT, signal.SIGTERM):
                loop.add_signal_handler(signum, engine.stop.set)
            result = await engine.run()
            return 2 if result in ("shortfall", "failed") else 0
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description="Sitemap-only persistent crawler")
    parser.add_argument("config", help="Path to YAML configuration")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    try:
        config = load_config(args.config)
        return asyncio.run(run(config))
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
