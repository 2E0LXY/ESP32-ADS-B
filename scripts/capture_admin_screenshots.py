"""Capture docs/screenshots/*.png from the mock admin server.

Replaces the previous Node/Playwright script. Requires:
    pip install playwright && playwright install chromium

Usage:
    python3 scripts/mock_admin_server.py 8123 &
    python3 scripts/capture_admin_screenshots.py
"""

import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8123"
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots"
VIEWS = ["overview", "aircraft", "mapview", "display", "wifi", "api", "device", "firmware"]
FILENAMES = {"mapview": "map"}

OUT.mkdir(parents=True, exist_ok=True)
with sync_playwright() as playwright:
    browser = playwright.chromium.launch()
    page = browser.new_page(viewport={"width": 1440, "height": 1000}, device_scale_factor=2)
    page.goto(BASE, wait_until="networkidle")
    for view in VIEWS:
        page.evaluate("id => document.querySelector(`[data-view=\"${id}\"]`)?.click()", view)
        page.wait_for_timeout(600)
        name = FILENAMES.get(view, view)
        page.screenshot(path=str(OUT / f"{name}.png"))
        print(f"captured {name}.png")
    browser.close()
