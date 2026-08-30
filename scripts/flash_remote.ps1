# Starts the esp_rfc2217_server.py serial bridge if it isn't already running,
# pulls the latest code, and builds+flashes over that bridge in one step.
#
# Run this from the repo root on the PC with the ESP32 attached by USB:
#   powershell -ExecutionPolicy Bypass -File scripts\flash_remote.ps1
#
# Common overrides:
#   -SerialPort COM5          # default: COM28
#   -PioEnv ws_lcd_7_app       # default: ws_lcd_7_app (use waveshare_esp32_s3_lcd_4 for the WS4 board)
#   -Monitor                  # skip the build/upload and just tail serial logs
#   -SkipPull                 # don't run "git pull" first

param(
  [string]$SerialPort = "COM28",
  [int]$TcpPort = 5555,
  [string]$PioEnv = "ws_lcd_7_app",
  [switch]$Monitor,
  [switch]$SkipPull
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Test-BridgeUp {
  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $TcpPort)
    $client.Close()
    return $true
  } catch {
    return $false
  }
}

$bridgeScript = Join-Path $repoRoot "esp_rfc2217_server.py"
if (-not (Test-Path $bridgeScript)) {
  Write-Host "Downloading esp_rfc2217_server.py..."
  Invoke-WebRequest -Uri "https://raw.githubusercontent.com/espressif/esptool/v4.6.2/esp_rfc2217_server.py" -OutFile $bridgeScript
}

if (Test-BridgeUp) {
  Write-Host "Serial bridge already running on TCP $TcpPort."
} else {
  Write-Host "Starting serial bridge: $SerialPort -> TCP $TcpPort ..."
  Start-Process -FilePath "python" -ArgumentList "`"$bridgeScript`" -p $TcpPort -v $SerialPort" -WindowStyle Normal
  $attempts = 0
  while (-not (Test-BridgeUp)) {
    Start-Sleep -Seconds 1
    $attempts++
    if ($attempts -gt 20) {
      throw "Serial bridge did not come up on TCP $TcpPort within 20s. Check the bridge window for errors (wrong COM port, port already in use, etc)."
    }
  }
  Write-Host "Bridge is up."
}

$uploadPort = "rfc2217://127.0.0.1:$TcpPort`?ign_set_control"

if ($Monitor) {
  pio device monitor -p $uploadPort -b 115200
  exit $LASTEXITCODE
}

if (-not $SkipPull) {
  git pull
}

pio run -e $PioEnv -t upload --upload-port $uploadPort
exit $LASTEXITCODE
