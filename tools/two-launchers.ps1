# Two launchers and two games on this PC: a host and a guest, each with its own launcher settings,
# data folder (bbport.ini, saves, logs) and install folder, for trying co-op alone exactly as two
# players would. Your own launcher settings, bbport.ini and saves are copied from, never changed.
# Put this file in the Awaken Seamless folder (beside Lantern Engine.exe):
#   Two-Launchers.cmd   then press Play in the HOST launcher first, then in the GUEST launcher.
param(
    [int]$Width = 960,
    [int]$Height = 540,
    [int]$HostPort = 9307,
    [int]$GuestPort = 9308
)
$ErrorActionPreference = 'Stop'
$package = $PSScriptRoot
if (!(Test-Path -LiteralPath (Join-Path $package 'Lantern Engine.exe'))) { throw 'Put this script in the Awaken Seamless folder.' }
$guestPackage = "$package-Guest"
$tests = Join-Path $package 'local-test'
$mine = Join-Path $env:APPDATA 'bloodborne-seamless\settings.json'
if (!(Test-Path -LiteralPath $mine)) { throw "Start Lantern Engine.exe once first (no $mine)." }

# The guest's own install: a second game may not run from the same folder (the launcher would close it).
if (!(Test-Path -LiteralPath (Join-Path $guestPackage 'Lantern Engine.exe')) -or
    (Get-Item (Join-Path $package 'Lantern Engine.exe')).LastWriteTime -gt (Get-Item (Join-Path $guestPackage 'Lantern Engine.exe')).LastWriteTime) {
    Write-Output "Copying the install for the guest to $guestPackage ..."
    robocopy $package $guestPackage /E /XD local-test user logs mods network engine-assets /XF Two-Launchers.cmd two-launchers.ps1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying the install failed (robocopy $LASTEXITCODE)." }
}

function Prepare([string]$role, [string]$install, [int]$port, [string]$name) {
    $data = Join-Path $tests $role
    $config = Join-Path $tests "$role-launcher"
    New-Item -ItemType Directory -Force -Path (Join-Path $data 'user'), $config | Out-Null
    # bbport.ini: your settings, in a small window.
    $lines = @(Get-Content -LiteralPath (Join-Path $package 'bbport.ini') |
        Where-Object { $_ -notmatch '^(output_res|fullscreen|upscaler|live_resolution)=' })
    $lines += "output_res=${Width}x$Height", 'fullscreen=0', 'upscaler=off', 'live_resolution=0'
    Set-Content -LiteralPath (Join-Path $data 'bbport.ini') -Value $lines -Encoding ascii
    # Launcher settings: yours, with this test's install, party role and ports.
    $settings = Get-Content -LiteralPath $mine -Raw | ConvertFrom-Json
    $values = [ordered]@{
        port_dir = $install; user_dir = ''; fullscreen = $false; check_updates = $false
        party_mode = $(if ($role -eq 'host') { 'host' } else { 'join' }); party_name = $name; party_max = '2'
        party_port = "$port"; party_upnp = $false; party_code = $(if ($role -eq 'host') { '' } else { "127.0.0.1:$HostPort" })
    }
    foreach ($key in $values.Keys) { $settings | Add-Member -NotePropertyName $key -NotePropertyValue $values[$key] -Force }
    [IO.File]::WriteAllText((Join-Path $config 'settings.json'), ($settings | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
    Remove-Item -LiteralPath (Join-Path $data 'user\party_command.json') -Force -ErrorAction SilentlyContinue
    return @{ Data = $data; Config = $config }
}

function Start-Launcher([string]$install, $paths, [string]$title) {
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $install 'Lantern Engine.exe'))
    $info.WorkingDirectory = $install
    $info.UseShellExecute = $false
    $info.EnvironmentVariables['BB_LAUNCHER_CONFIG_DIR'] = $paths.Config
    $info.EnvironmentVariables['BB_DATA_DIR'] = $paths.Data
    [Diagnostics.Process]::Start($info) | Out-Null
    Write-Output "$title launcher started (data: $($paths.Data))"
}

$hostPaths = Prepare 'host' $package $HostPort 'HostTest'
$guestPaths = Prepare 'guest' $guestPackage $GuestPort 'GuestTest'
Start-Launcher $package $hostPaths 'HOST'
Start-Sleep -Seconds 2
Start-Launcher $guestPackage $guestPaths 'GUEST'
Write-Output ''
Write-Output 'Press Play in the HOST launcher; once its game is at the lamp, press Play in the GUEST launcher.'
Write-Output "The guest is set to join 127.0.0.1:$HostPort. Input goes to the game window you click."
