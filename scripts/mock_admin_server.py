"""Serve src/web_ui.h against canned API responses for screenshot capture.

Replaces the previous Node implementation; stdlib only, no npm dependency.
Usage: python3 scripts/mock_admin_server.py [port]
"""

import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGE = re.search(
    r'R"ADSBWEB\((.*)\)ADSBWEB";',
    (ROOT / "src" / "web_ui.h").read_text(encoding="utf-8"),
    re.S,
).group(1)

STATUS = {
    "firmware": "2.5.1", "provider": "adsb.fi", "aircraftCount": 42,
    "brightness": 80, "displayPage": 0, "homeLatitude": 53.6833,
    "homeLongitude": -1.4977, "radiusNm": 120, "mapZoom": 8,
    "rssi": -54, "heapFree": 214000, "uptimeSeconds": 48210,
    "sdMounted": True, "sdCardType": "SDHC", "sdTotalBytes": 31914983424,
    "sdUsedBytes": 1203765248, "physicalMapReady": True,
    "mapRebuildActive": False, "mapRebuildDone": 0, "mapRebuildTotal": 0,
    "githubUpdateAvailable": False, "githubLatestVersion": "v2.5.1",
    "githubUpdateStatus": "Firmware is current",
    "releaseRepository": "2E0LXY/ESP32-ADS-B",
    "csrfToken": "0123456789abcdef0123456789abcdef",
}

AIRCRAFT = {"aircraft": [
    {"hex": "406a3d", "callsign": "BAW117", "registration": "G-STBH",
     "aircraftType": "B77W", "distance": 18.4, "lat": 53.71, "lon": -1.42,
     "alt": 34000, "geomAlt": 34350, "speed": 461, "verticalRate": 0,
     "heading": 96, "direction": "E", "squawk": "6412", "category": "A5",
     "onGround": False, "age": 2.1, "messages": 1840, "signal": -14.2,
     "country": "United Kingdom", "route": "LHR>DXB", "source": "adsb",
     "emergency": "", "operator": "British Airways"},
]}


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, content_type):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path.startswith("/api/status"):
            self._send(200, json.dumps(STATUS), "application/json")
        elif self.path.startswith("/api/aircraft"):
            self._send(200, json.dumps(AIRCRAFT), "application/json")
        elif self.path.startswith("/api/wifi/scan"):
            self._send(200, json.dumps({"scanning": False, "networks": []}), "application/json")
        else:
            self._send(200, PAGE, "text/html; charset=utf-8")

    def do_POST(self):
        self._send(200, json.dumps({"message": "ok"}), "application/json")

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8123
    print(f"Mock admin UI on http://127.0.0.1:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
