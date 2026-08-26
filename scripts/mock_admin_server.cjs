const http = require("http");
const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(path.join(__dirname, "..", "src", "web_ui.h"), "utf8");
const match = source.match(/R"ADSBWEB\(([\s\S]*)\)ADSBWEB";/);
if (!match) throw new Error("Unable to extract WEB_UI");

const status = {
  aircraftTotal: 2, adsb: 1, mlat: 1, credits: 3900, lastRefreshSeconds: 4,
  feedStatus: "OK", feedHttpCode: 200, feedDurationMs: 812, rssi: -51,
  uptimeSeconds: 86420, ssid: "Test Wi-Fi", ip: "192.168.1.74", hostname: "adsb-map",
  provider: "adsblol", hasOpenSkyClientId: false, hasOpenSkyClientSecret: false,
  hasRapidApiKey: false, version: "2.5.0", build: "Aug 26 2026 18:00:00",
  updateSpace: 6311936, brightness: 80, sound: true, page: "map",
  latitude: 53.73, longitude: -1.57, radiusNm: 60, mapZoom: 9,
  updateAvailable: true, latestVersion: "2.5.1", updateStatus: "Version 2.5.1 available",
  releaseRepository: "2E0LXY/ESP32-ADS-B", heapFree: 180000, heapMinimum: 126000,
  heapLargest: 112000, loopStackMinimumFree: 7200, psramFree: 6900000,
  psramMinimum: 6400000, psramLargest: 6000000, temperatureC: 43.2,
  aircraftCapacity: 250, aircraftStorage: "PSRAM", sdMounted: true,
  sdStatus: "Ready", sdType: "SDHC/SDXC", sdTotalBytes: 15931539456,
  sdUsedBytes: 104857600, sdFreeBytes: 15826681856, tileCacheStorage: "SD card",
  stagedUpdateReady: false, stagedUpdateVersion: ""
};
const aircraft = { aircraft: [
  { hex:"40621b", callsign:"BAW123", latitude:53.8, longitude:-1.5, distance:6.2,
    altitude:12000, geometricAltitude:12300, speed:312, verticalRate:500, heading:140,
    direction:"SE", source:"ADSB", registration:"G-EUYA", aircraftType:"A320",
    squawk:"7000", category:"A3", operator:"British Airways", country:"United Kingdom",
    emergency:"none", onGround:false, age:0.4, messages:2321, signal:-18.2, route:"LHR>EDI" },
  { hex:"4ca123", callsign:"RYR45AB", latitude:53.6, longitude:-1.7, distance:10.1,
    altitude:9000, geometricAltitude:9250, speed:280, verticalRate:-700, heading:315,
    direction:"NW", source:"MLAT", registration:"EI-ABC", aircraftType:"B738",
    squawk:"4452", category:"A3", operator:"Ryanair", country:"Ireland",
    emergency:"none", onGround:false, age:1.2, messages:980, signal:-24.1, route:"DUB>MAN" }
]};

http.createServer((request, response) => {
  response.setHeader("Cache-Control", "no-store");
  if (request.url.startsWith("/api/status")) return response.end(JSON.stringify(status));
  if (request.url.startsWith("/api/aircraft")) return response.end(JSON.stringify(aircraft));
  if (request.url.startsWith("/api/")) return response.end(JSON.stringify({ message: "Mock action accepted" }));
  response.setHeader("Content-Type", "text/html; charset=utf-8");
  response.end(match[1]);
}).listen(8765, "127.0.0.1", () => console.log("Mock admin: http://127.0.0.1:8765/"));
