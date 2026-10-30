# Two copies of the game on this PC, a host and a guest that joins it, in small windows side by side:
# for trying co-op alone. Each copy has its own test profile in local-test\ (saves, settings, logs);
# the player's own saves are not used. Put this file in the package folder (beside Nightwatch Co-op.exe).
#   Play-Two.cmd                  (or)  powershell -ExecutionPolicy Bypass -File play-two.ps1 -Width 960 -Height 540
# Input goes to the window that has focus: click a window to play it.
param(
    [string]$Game = '',
    [int]$Width = 960,
    [int]$Height = 540
)
$ErrorActionPreference = 'Stop'
$package = $PSScriptRoot
$start = Join-Path $package 'engine\out\bb-start.exe'
if (!(Test-Path -LiteralPath $start)) { throw "Put this script in the Nightwatch Co-op folder (no engine\out\bb-start.exe beside it)." }
if (!$Game) {
    $settings = Join-Path $env:APPDATA $(if (Test-Path (Join-Path $env:APPDATA 'nightwatch')) { 'nightwatch\settings.json' } else { 'bloodborne-seamless\settings.json' })
    if (Test-Path -LiteralPath $settings) { $Game = (Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).game_dir }
}
if (!$Game -or !(Test-Path -LiteralPath (Join-Path $Game 'eboot.bin'))) { throw "Game folder not found; pass -Game <folder with eboot.bin>." }

Add-Type -Namespace Win32 -Name Window -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr hWnd, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
'@

$root = Join-Path $package 'local-test'
$code = Join-Path $root 'host-code.txt'
New-Item -ItemType Directory -Force -Path $root | Out-Null
Remove-Item -LiteralPath $code -Force -ErrorAction SilentlyContinue

function Start-Copy([string]$role, [string]$name, [int]$port) {
    $dir = Join-Path $root $role
    $user = Join-Path $dir 'user'
    New-Item -ItemType Directory -Force -Path $user | Out-Null
    # The player's own settings, in a small window.
    $ini = Join-Path $dir 'bbport.ini'
    $lines = @(Get-Content -LiteralPath (Join-Path $package 'bbport.ini') |
        Where-Object { $_ -notmatch '^(output_res|fullscreen|upscaler|live_resolution)=' })
    $lines += "output_res=${Width}x$Height", 'fullscreen=0', 'upscaler=off', 'live_resolution=0'
    Set-Content -LiteralPath $ini -Value $lines -Encoding ascii
    foreach ($file in 'party_command.json', 'party_status.json') { Remove-Item -LiteralPath (Join-Path $user $file) -Force -ErrorAction SilentlyContinue }
    if (!(Test-Path -LiteralPath (Join-Path $user 'savedata_party'))) {
        Write-Warning "$role has no party character yet: make one at the title screen (New Game)."
    }
    $info = [Diagnostics.ProcessStartInfo]::new($start)
    $info.Arguments = "--game-dir `"$Game`""
    $info.WorkingDirectory = Join-Path $package 'engine'
    $info.UseShellExecute = $false
    $vars = $info.EnvironmentVariables
    foreach ($key in @($vars.Keys)) { if ($key -like "BB_*") { $vars.Remove($key) } }
    $values = @{
        BB_DATA_DIR = $dir; BB_CONFIG = $ini; BB_USER_DIR = $user; BB_GAME_DIR = $Game; BB_LOG_FILE = (Join-Path $dir 'session.log')
        BB_PARTY = $role.Replace('guest', 'join'); BB_PARTY_NAME = $name; BB_PARTY_MAX = '2'; BB_PARTY_PORT = "$port"
        BB_PARTY_CODE_FILE = $code; BB_PARTY_LOOPBACK = '1'; BB_PARTY_LOCAL_IP = '127.0.0.1'; BB_PARTY_UPNP = '0'; BB_PARTY_STUN = 'off'
        BB_PARTY_AUTOCONTINUE = '1'; BB_FULLSCREEN = '0'; BB_GPU_CACHE_DIR = (Join-Path $user 'gpu-cache')
    }
    foreach ($key in $values.Keys) { $vars[$key] = $values[$key] }
    $process = [Diagnostics.Process]::Start($info)
    Write-Output "$role started (log: $dir\session.log)"
    return $process
}

# The game window of a bb-start: its bb-probe child's main window.
function Get-GameWindow($startProcess) {
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($startProcess.Id) AND Name='bb-probe.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$child) { return [IntPtr]::Zero }
    $probe = Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue
    if ($probe) { return $probe.MainWindowHandle } else { return [IntPtr]::Zero }
}

function Wait-AndPlace($startProcess, [int]$x, [string]$role) {
    for ($i = 0; $i -lt 240; $i++) {
        if ($startProcess.HasExited) { Write-Warning "$role ended (exit $($startProcess.ExitCode)); see its session.log"; return }
        $window = Get-GameWindow $startProcess
        if ($window -ne [IntPtr]::Zero) {
            [Win32.Window]::SetWindowPos($window, [IntPtr]::Zero, $x, 40, 0, 0, 0x0001 -bor 0x0004) | Out-Null  # NOSIZE, NOZORDER
            Write-Output "$role window placed"
            return
        }
        Start-Sleep -Milliseconds 500
    }
}

$host_ = Start-Copy 'host' 'HostTest' 47901
Wait-AndPlace $host_ 0 'host'
Write-Output 'Waiting for the host to open the party (its party code)...'
for ($i = 0; $i -lt 240 -and !(Test-Path -LiteralPath $code) -and !$host_.HasExited; $i++) { Start-Sleep -Milliseconds 500 }
if (!(Test-Path -LiteralPath $code)) { throw 'The host did not open the party; see local-test\host\session.log.' }
$guest = Start-Copy 'guest' 'GuestTest' 47902
Wait-AndPlace $guest ($Width + 20) 'guest'
Write-Output 'Both copies are running: the guest joins the host once both are in the game. Close a window to end that copy.'
