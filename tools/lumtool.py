#!/usr/bin/env python3
"""DriverPoE admin tool -- thin CLI entry point.

All the actual logic (protocol, client, discovery, models, secret
storage, and the interactive menu itself) lives in the driverpoe/
package next to this file -- see driverpoe/cli.py for the menu and
driverpoe/README (package docstring, driverpoe/__init__.py) for the
public API, which is what tools/webui/ also builds on.

Run with no arguments for the interactive menu:

    python tools/lumtool.py

Run the test suite from inside tools/:

    python -m unittest discover -s driverpoe/tests -v
"""
from __future__ import annotations

from driverpoe.cli import main

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye.")
