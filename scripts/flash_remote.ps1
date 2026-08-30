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
#   -InstallStartupTask       # register a Task Scheduler entry that starts the
#                             # bridge at logon, so it survives reboots/logouts
#                             # and you never have to start it by hand again
#   -UninstallStartupTask     # remove that task

param(
  [string]$SerialPort = "COM28",
  [int]$TcpPort = 5555,
  [string]$PioEnv = "ws_lcd_7_app",
  [switch]$Monitor,
  [switch]$SkipPull,
  [switch]$InstallStartupTask,
  [switch]$UninstallStartupTask
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot
$taskName = "ESP32ADSB-SerialBridge"

if ($UninstallStartupTask) {
  schtasks /Delete /TN $taskName /F 2>$null
  Write-Host "Removed scheduled task '$taskName' (if it existed)."
  exit 0
}

if ($InstallStartupTask) {
  $bridgeScript = Join-Path $repoRoot "esp_rfc2217_server.py"
  if (-not (Test-Path $bridgeScript)) {
    Write-Host "Downloading esp_rfc2217_server.py..."
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/espressif/esptool/v4.6.2/esp_rfc2217_server.py" -OutFile $bridgeScript
  }
  $pythonw = (Get-Command pythonw -ErrorAction SilentlyContinue)
  $exe = if ($pythonw) { $pythonw.Source } else { (Get-Command python).Source }
  $action = "`"$exe`" `"$bridgeScript`" -p $TcpPort -v $SerialPort"
  schtasks /Create /TN $taskName /TR $action /SC ONLOGON /RL LIMITED /F | Out-Null
  Write-Host "Installed scheduled task '$taskName': starts the bridge on $SerialPort -> TCP $TcpPort at logon."
  Write-Host "Starting it now for this session too..."
  schtasks /Run /TN $taskName | Out-Null
  exit 0
}

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
