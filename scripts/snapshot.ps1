param(
  [string]$Source = 'C:\Users\2e0lx\ESP32-ADS-B-new',
  [string]$Root   = 'D:\Backups\ESP32-ADS-B\snapshots',
  [int]$Keep      = 15
)

# Timestamped copy of the working tree, .git and untracked files included.
# The mirror remote covers committed history; this covers everything that
# has not been committed yet, which is what was actually lost.

$ErrorActionPreference = 'Stop'

try {
  $stamp  = Get-Date -Format 'yyyy-MM-dd_HHmmss'
  $sha    = (git -C $Source rev-parse --short HEAD 2>$null)
  if ($sha) { $stamp = "$stamp`_$sha" }
  $target = Join-Path $Root $stamp

  # /MIR mirrors, /XD excludes build output, /NFL /NDL /NJH /NJS quiet it.
  # Exit codes 0-7 are success for robocopy; 8+ is a real failure.
  robocopy $Source $target /MIR /XD '.pio' 'node_modules' /R:1 /W:1 /NFL /NDL /NJH /NJS | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy exit $LASTEXITCODE" }

  # Keep the most recent $Keep snapshots; drop the rest oldest-first.
  Get-ChildItem -Path $Root -Directory |
    Sort-Object Name -Descending |
    Select-Object -Skip $Keep |
    ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }

  Write-Output "snapshot: $target"
}
catch {
  Write-Output "snapshot failed: $_"
  exit 1
}
