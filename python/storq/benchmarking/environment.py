import platform
import sqlite3
import sys
from pathlib import Path


def environment_fingerprint(database_engine: str | None = None, config: dict | None = None) -> dict:
    cpu_count = None
    available_memory = None
    try:
        import os
        cpu_count = os.cpu_count()
        if hasattr(__import__("psutil"), "virtual_memory"):
            available_memory = __import__("psutil").virtual_memory().available
    except (ImportError, AttributeError, OSError):
        pass
    return {
        "os": platform.system(),
        "os_version": platform.release(),
        "cpu_architecture": platform.machine(),
        "processor_count": cpu_count,
        "available_memory_bytes": available_memory,
        "python_version": sys.version.split()[0],
        "storq_version": "0.1.0",
        "sqlite_version": sqlite3.sqlite_version,
        "database_engine": database_engine,
        "configuration": dict(config or {}),
    }
