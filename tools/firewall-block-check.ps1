<#
Proves that the launcher's "Fix firewall issues" script removes the game's inbound block rules and adds
its allow rules. Needs an administrator PowerShell; exits 2 (skip) when not elevated.

    powershell -NoProfile -ExecutionPolicy Bypass -File tools\firewall-block-check.ps1 -ScriptFile <file> -Exe <dummy exe> -Port <port>

<file> is FirewallFix.Script(port, exe) as printed by `dotnet run --project SeamlessTests -- --firewall-script <port> <exe>`.
Use a dummy exe path and a port nobody uses: every rule this check creates is removed again, and the shared
party discovery rule is removed only when it did not exist before.
#>
param(
    [Parameter(Mandatory)] [string] $ScriptFile,
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [int] $Port
)
$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Output 'SKIP: not elevated (run from an administrator shell to prove the block-rule removal)'
    exit 2
}

$blockName = 'Nightwatch QA block check'
$allowNames = @("Nightwatch Co-op party (TCP) $Port", "Nightwatch Co-op party (UDP) $Port")
$discovery = Get-Content -Raw $ScriptFile | Select-String -Pattern "Nightwatch Co-op party discovery \(UDP\) \d+" |
    ForEach-Object { $_.Matches[0].Value }
$discoveryExisted = [bool](Get-NetFirewallRule -DisplayName $discovery -ErrorAction SilentlyContinue)

function Block-Rules {
    @(Get-NetFirewallApplicationFilter | Where-Object Program -eq $Exe | Get-NetFirewallRule |
        Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Block' })
}

$failed = @()
try {
    New-NetFirewallRule -DisplayName $blockName -Direction Inbound -Program $Exe -Action Block -Profile Any | Out-Null
    if ((Block-Rules).Count -lt 1) { throw 'the test block rule was not created' }

    & ([scriptblock]::Create((Get-Content -Raw $ScriptFile)))

    if ((Block-Rules).Count -ne 0) { $failed += 'inbound block rule for the exe still there' }
    foreach ($name in $allowNames + $discovery) {
        $rule = Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue
        if (-not $rule -or $rule.Action -ne 'Allow' -or $rule.Direction -ne 'Inbound' -or $rule.Enabled -ne 'True') {
            $failed += "allow rule missing: $name"
        }
    }
}
finally {
    Remove-NetFirewallRule -DisplayName $blockName -ErrorAction SilentlyContinue
    foreach ($name in $allowNames) { Remove-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue }
    if (-not $discoveryExisted) { Remove-NetFirewallRule -DisplayName $discovery -ErrorAction SilentlyContinue }
}

if ($failed.Count) {
    Write-Output ('FAIL: ' + ($failed -join '; '))
    exit 1
}
Write-Output "PASS: block rule removed, $($allowNames.Count + 1) allow rules present, test rules cleaned up"
exit 0
