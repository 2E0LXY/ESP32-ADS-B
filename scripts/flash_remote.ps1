# Starts the esp_rfc2217_server.py serial bridge if it isn't already running,
# pulls the latest code, and builds+flashes over that bridge in one step.
#
# Run this from the repo root on the PC with the ESP32 attached by USB:
#   powershell -ExecutionPolicy Bypass -File scripts\flash_remote.ps1
#
# Common overrides:
#   -SerialPort COM5          # override auto-detection and force a specific port
#   -PioEnv ws_lcd_7_app       # default: ws_lcd_7_app (use waveshare_esp32_s3_lcd_4 for the WS4 board)
#   -Monitor                  # skip the build/upload and just tail serial logs
#   -SkipPull                 # don't run "git pull" first
#   -InstallStartupTask       # register a Task Scheduler entry that starts the
#                             # bridge at logon, so it survives reboots/logouts
#                             # and you never have to start it by hand again
#   -UninstallStartupTask     # remove that task

param(
  [string]$SerialPort,
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

# COM port numbers drift (a different USB port/hub, another device grabbing
# one first), so hunt for the actual USB-serial adapter instead of hardcoding
# a port that stops matching reality. The WS7 board's UART bridge chip shows
# up in Device Manager as one of these controller names.
function Find-SerialPort {
  $candidates = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'CH340|CH343|CH9102|CP210|FTDI|FT232|USB-SERIAL|USB Serial' -and $_.Name -match '\(COM(\d+)\)' } |
    ForEach-Object {
      if ($_.Name -match '\((COM\d+)\)') { [PSCustomObject]@{ Port = $matches[1]; Name = $_.Name } }
    }
  return @($candidates)
}

if (-not $SerialPort) {
  $found = Find-SerialPort
  if ($found.Count -eq 1) {
    $SerialPort = $found[0].Port
    Write-Host "Auto-detected serial port: $SerialPort ($($found[0].Name))"
  } elseif ($found.Count -gt 1) {
    Write-Host "Multiple USB-serial adapters found - pass -SerialPort to pick one:"
    $found | ForEach-Object { Write-Host "  $($_.Port): $($_.Name)" }
    throw "Ambiguous serial port; re-run with -SerialPort COMx"
  } else {
    throw "No USB-serial adapter auto-detected. Plug in the board, or check Device Manager > Ports (COM & LPT) and pass -SerialPort COMx explicitly."
  }
}

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
$uploadExitCode = $LASTEXITCODE

if ($uploadExitCode -eq 0) {
  # ign_set_control (needed above to stop the bridge dropping the connection
  # mid-sync) also stops esptool's normal DTR/RTS reset pulse from reaching
  # the board, so a successful upload can leave it sitting in the ROM
  # bootloader instead of running the new firmware. Try a plain reset - no
  # flashing, just the reset pulse - over a connection that allows control
  # lines; if the bridge still won't carry them, this is a no-op and you'll
  # need to power-cycle the board by hand, same as before this existed.
  Write-Host "Upload succeeded; attempting to reset the board..."
  try {
    python -m esptool --chip esp32s3 --port "rfc2217://127.0.0.1:$TcpPort" --before default_reset --after hard_reset chip_id 2>&1 | Out-Null
    Write-Host "Reset attempted. If the screen is still blank, power-cycle the board."
  } catch {
    Write-Host "Reset attempt failed ($($_.Exception.Message)); power-cycle the board to run the new firmware."
  }
}

exit $uploadExitCode
