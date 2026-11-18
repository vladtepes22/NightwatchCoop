# lock.ps1 take|release: the machine-wide game lock. take: exit 0 when this run now holds it (and free commit >= 10 GB),
# 1 otherwise. A lock older than 20 min while no bb-probe runs is stale and removed.
param([string]$op, [string]$run = '')
# One path every session and worktree of this repo sees: the main checkout's work folder (BB_GAME_LOCK names another).
$L = if ($env:BB_GAME_LOCK) { $env:BB_GAME_LOCK } else {
  Join-Path (Split-Path (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir)) 'work\game-runs.lock' }
$me = 'solo play tester'
if ($op -eq 'release') {
  if ((Test-Path $L) -and ((Get-Content $L -Raw) -like "$me*")) { Remove-Item $L -Force }
  exit 0
}
$free = (Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB
try {
  New-Item -ItemType File -Path $L -ErrorAction Stop | Out-Null
} catch {
  $f = Get-Item $L -ErrorAction SilentlyContinue
  if ($f -and ((Get-Date) - $f.LastWriteTime).TotalMinutes -gt 20 -and -not (Get-Process bb-probe -ErrorAction SilentlyContinue)) {
    Remove-Item $L -Force -ErrorAction SilentlyContinue
  }
  exit 1
}
if ($free -lt 10) { Remove-Item $L -Force; "free commit {0:N1} GB: waiting" -f $free; exit 1 }
[IO.File]::WriteAllText($L, "$me ($run), started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), free commit $([math]::Round($free,1)) GB")
exit 0
