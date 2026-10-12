#!/usr/bin/env python3
"""Stable executable entry point used by the CloudCompare plugin."""

# Keep the signed macOS application immutable at runtime.
import sys
sys.dont_write_bytecode = True

from qal_ml.cli import main


if __name__ == "__main__":
    raise SystemExit(main())
