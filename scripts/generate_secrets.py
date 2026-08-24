Import("env")

import json
from pathlib import Path

credentials_path = Path.home() / "Desktop" / "credentials.json"
generated_dir = Path(env["PROJECT_DIR"]) / ".pio" / "generated"
generated_header = generated_dir / "opensky_secrets.h"

credentials = {}
if credentials_path.exists():
    with credentials_path.open("r", encoding="utf-8") as stream:
        credentials = json.load(stream)

client_id = credentials.get("clientId", "")
client_secret = credentials.get("clientSecret", "")

def cpp_string(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')

generated_dir.mkdir(parents=True, exist_ok=True)
generated_header.write_text(
    "#pragma once\n"
    f'static constexpr char OPENSKY_CLIENT_ID[] = "{cpp_string(client_id)}";\n'
    f'static constexpr char OPENSKY_CLIENT_SECRET[] = "{cpp_string(client_secret)}";\n',
    encoding="utf-8",
)

env.Append(CPPPATH=[str(generated_dir)])
