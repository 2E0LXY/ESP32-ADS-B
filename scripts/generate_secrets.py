"""Emit opensky_secrets.h.

By default this writes EMPTY credentials. Credentials are only baked into the
binary when the build defines ADSB_BAKE_CREDENTIALS, because anything compiled
in is recoverable from a distributed image with `strings firmware.bin`.

Set ADSB_CREDENTIALS_FILE to point at the JSON file, or place it at
credentials.json beside platformio.ini. Both paths are gitignored.
"""

Import("env")

import json
import os
from pathlib import Path

project_dir = Path(env["PROJECT_DIR"])
generated_dir = project_dir / ".pio" / "generated"
generated_header = generated_dir / "opensky_secrets.h"

bake = any("ADSB_BAKE_CREDENTIALS" in str(flag) for flag in env.get("CPPDEFINES", []))

credentials = {}
if bake:
    candidates = []
    override = os.environ.get("ADSB_CREDENTIALS_FILE")
    if override:
        candidates.append(Path(override))
    candidates.append(project_dir / "credentials.json")
    candidates.append(Path.home() / "Desktop" / "credentials.json")
    for candidate in candidates:
        if candidate.exists():
            with candidate.open("r", encoding="utf-8") as stream:
                credentials = json.load(stream)
            print(f"generate_secrets: baking credentials from {candidate}")
            break
    else:
        print("generate_secrets: ADSB_BAKE_CREDENTIALS set but no credentials file found")


def cpp_string(value):
    return str(value).replace("\\", "\\\\").replace('"', '\\"')


generated_dir.mkdir(parents=True, exist_ok=True)
generated_header.write_text(
    "#pragma once\n"
    f'static constexpr char OPENSKY_CLIENT_ID[] = "{cpp_string(credentials.get("clientId", ""))}";\n'
    f'static constexpr char OPENSKY_CLIENT_SECRET[] = "{cpp_string(credentials.get("clientSecret", ""))}";\n',
    encoding="utf-8",
)

env.Append(CPPPATH=[str(generated_dir)])
