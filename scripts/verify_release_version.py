"""Fail a release build when the tag and user-visible versions disagree."""

import json
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def match_version(path: Path, pattern: str) -> str:
    content = path.read_text(encoding="utf-8")
    match = re.search(pattern, content)
    if not match:
        raise SystemExit(f"Could not find a firmware version in {path.relative_to(ROOT)}")
    return match.group(1)


firmware_version = match_version(
    ROOT / "src" / "main.cpp",
    r'FIRMWARE_VERSION\[\]\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
)
manifest_version = json.loads((ROOT / "docs" / "manifest.json").read_text(encoding="utf-8"))["version"]
readme_version = match_version(ROOT / "README.md", r"Current firmware:\s*\*\*v([0-9]+\.[0-9]+\.[0-9]+)\*\*")
installer_version = match_version(ROOT / "docs" / "index.html", r"Firmware</span><span>v([0-9]+\.[0-9]+\.[0-9]+)</span>")

versions = {
    "firmware": firmware_version,
    "manifest": manifest_version,
    "README": readme_version,
    "installer": installer_version,
}
if len(set(versions.values())) != 1:
    raise SystemExit("Version mismatch: " + ", ".join(f"{name}={version}" for name, version in versions.items()))

tag = os.environ.get("RELEASE_TAG", "").removeprefix("v")
if tag and tag != firmware_version:
    raise SystemExit(f"Release tag v{tag} does not match firmware v{firmware_version}")

print(f"Version check passed: v{firmware_version}")
