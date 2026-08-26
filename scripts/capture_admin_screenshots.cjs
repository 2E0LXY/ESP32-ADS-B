const path = require("path");
const fs = require("fs");
const { chromium } = require("playwright");

const baseUrl = process.env.ADSB_SCREENSHOT_URL || "http://192.168.1.74/";
const username = process.env.ADSB_SCREENSHOT_USERNAME || "admin";
const password = process.env.ADSB_SCREENSHOT_PASSWORD || "aircraft";
const installerUrl = process.env.ADSB_INSTALLER_URL || "https://2e0lxy.github.io/ESP32-ADS-B/";
const outputDirectory = path.resolve(__dirname, "..", "docs", "screenshots");
const views = ["overview", "mapview", "aircraft", "display", "wifi", "api", "firmware", "device"];

async function main() {
  fs.mkdirSync(outputDirectory, { recursive: true });
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    httpCredentials: { username, password },
    viewport: { width: 1920, height: 1080 },
    deviceScaleFactor: 1,
    colorScheme: "dark",
  });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", error => errors.push(error.message));

  for (const view of views) {
    await page.goto(`${baseUrl}#${view}`, { waitUntil: "domcontentloaded" });
    await page.waitForSelector(`#${view}:not(.view-hidden)`, { timeout: 15000 });
    await page.waitForFunction(() => {
      const version = document.querySelector("#footerVersion")?.textContent || "";
      const total = document.querySelector("#total")?.textContent || "";
      return version !== "v--" && total !== "--";
    }, null, { timeout: 20000 });
    if (view === "mapview") {
      await page.waitForFunction(() => document.querySelectorAll(".leaflet-tile-loaded").length > 0, null, { timeout: 20000 }).catch(() => {});
      await page.waitForFunction(() => document.querySelectorAll(".leaflet-marker-icon").length > 0, null, { timeout: 20000 }).catch(() => {});
      await page.waitForTimeout(1500);
    }
    await page.evaluate(() => {
      const ssid = document.querySelector("#ssid");
      if (ssid) ssid.textContent = "Home Wi-Fi";
    });
    const filename = view === "mapview" ? "map.png" : `${view}.png`;
    await page.screenshot({ path: path.join(outputDirectory, filename), fullPage: true });
  }

  const installer = await context.newPage();
  await installer.goto(installerUrl, { waitUntil: "networkidle" });
  await installer.waitForSelector("esp-web-install-button", { timeout: 15000 });
  await installer.screenshot({ path: path.join(outputDirectory, "installer.png"), fullPage: true });

  await browser.close();
  if (errors.length) throw new Error(`Browser errors:\n${errors.join("\n")}`);
  console.log(`Captured ${views.length + 1} screenshots in ${outputDirectory}`);
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
