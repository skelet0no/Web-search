"""Independent analysis configuration; crawler scheduler settings are not required."""

from pathlib import Path

import yaml


def load_config(path, uri=None):
    with Path(path).open(encoding="utf-8") as stream:
        config = yaml.safe_load(stream)
    if not isinstance(config, dict) or not isinstance(config.get("db"), dict):
        raise ValueError("Configuration requires a db mapping")
    db = dict(config["db"])
    if uri:
        db["uri"] = uri
    if not isinstance(db.get("uri"), str) or not db["uri"].startswith(("mongodb://", "mongodb+srv://")):
        raise ValueError("db.uri must be a MongoDB URI")
    if not isinstance(db.get("database"), str) or not db["database"].strip():
        raise ValueError("db.database is required")
    sources = config.get("sources", [])
    if not isinstance(sources, list) or any(
        not isinstance(s, dict) or not isinstance(s.get("name"), str) for s in sources
    ):
        raise ValueError("sources must be a list of mappings with names")
    return {"db": db, "sources": sources}
