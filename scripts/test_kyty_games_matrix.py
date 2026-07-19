#!/usr/bin/env python3
"""Run the canonical matrix integration suite from the toolkit verifier."""

from __future__ import annotations

import runpy
from pathlib import Path


if __name__ == "__main__":
    suite = Path(__file__).resolve().parents[1] / "source" / "integration_test" / "TestGamesMatrix.py"
    runpy.run_path(str(suite), run_name="__main__")
