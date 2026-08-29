"""Puts the private analytics modules on the import path.

The max-HP solver is a faithful port of SourceFiles/MaxHpSolver.cpp and lives in the private
repository beside it, so the two can be changed together and neither is published on its own.
Everything that needs it imports this module first.

A missing private checkout is a DEPLOYMENT fault, not a data fault, and it has to say so out
loud: upload_to_r2 wraps the analytics call in a broad try/except on purpose, so that a parser
defect never costs a match its index entry - which also means a bare ImportError here would be
swallowed and every match would publish with no combat_analytics at all, leaving one warning
line in the log as the only trace.
"""
import sys
from pathlib import Path

PRIVATE_SCRIPTS = Path(__file__).resolve().parent.parent.parent / "gwobserver-private" / "scripts"


def ensure() -> Path:
    """Insert the private scripts directory on sys.path, or explain why it is not there."""
    if not PRIVATE_SCRIPTS.is_dir():
        raise RuntimeError(
            f"the private analytics modules are missing: {PRIVATE_SCRIPTS} does not exist.\n"
            "Clone https://github.com/mevi826/gwobserver-private next to this repository, so the "
            "two sit side by side. Without it every match publishes with no combat_analytics."
        )
    path = str(PRIVATE_SCRIPTS)
    if path not in sys.path:
        sys.path.insert(0, path)
    return PRIVATE_SCRIPTS


ensure()
