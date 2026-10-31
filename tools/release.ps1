# Publishes a release whose package is already built and tested: checks the package, the repo and
# the README, runs the privacy scan, then tags, pushes and creates the GitHub prerelease.
# Build, tests and packaging stay separate steps (CLAUDE.md, Workflows).
#   tools\release.ps1 -Package B:\work\package-b3\NightwatchCoop -Notes work\release\RELEASE_NOTES_0.2.3b1.md [-DryRun]
# The privacy patterns (your username, friends' names, anything that must not ship) are read from
# %USERPROFILE%\.nightwatch-privacy-patterns.txt, one per line. They stay out of the repo on purpose.
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Notes,
    [string]$Release = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not $Release) { $Release = Join-Path $repo 'work\release' }
$branch = (git -C $repo rev-parse --abbrev-ref HEAD).Trim()
$remote = 'fork'
$repoName = 'vladtepes22/NightwatchCoop'
$gh = 'C:\msys64\clang64\bin\gh.exe'
$version = (Get-Content -LiteralPath (Join-Path $repo 'VERSION') -Raw).Trim()
$tag = "v$version"
$title = 'Nightwatch Co-op ' + ($version -replace '-beta\.(\d+)$', ' beta $1')
$zip = Join-Path $Release "NightwatchCoop-$tag-win64.zip"
$exe = Join-Path $Package 'Nightwatch Co-op.exe'
$manifest = Join-Path $Package 'package.json'
$patternsFile = Join-Path $env:USERPROFILE '.nightwatch-privacy-patterns.txt'

$problems = New-Object System.Collections.Generic.List[string]

# Release files and notes. The launcher and manifest come from the package they were built in.
foreach ($file in @($zip, $exe, $manifest, $Notes)) {
    if (-not (Test-Path -LiteralPath $file)) { $problems.Add("missing file: $file") }
}
$head = (git -C $repo rev-parse HEAD).Trim()
$packageManifest = Join-Path $Package 'package.json'
if (-not (Test-Path -LiteralPath $packageManifest)) {
    $problems.Add("no package.json in $Package")
} else {
    $pkg = Get-Content -LiteralPath $packageManifest -Raw | ConvertFrom-Json
    if ($pkg.version -ne $version) { $problems.Add("package is version $($pkg.version), VERSION says $version") }
    if ($pkg.source -ne $head) { $problems.Add("package was built from $($pkg.source), HEAD is ${head}: rebuild the package") }
}

# Clean repo (the fsr-vulkan submodule always shows as modified; it is never committed).
$dirty = @(git -C $repo status --porcelain --ignore-submodules=dirty)
if ($dirty.Count) { $problems.Add("uncommitted changes: $($dirty[0]) (+$($dirty.Count - 1) more)") }

# Tag not used yet, locally or on the remote.
if (git -C $repo tag -l $tag) { $problems.Add("tag $tag already exists locally") }
$remoteTag = git -C $repo ls-remote --tags $remote "refs/tags/$tag"
if ($LASTEXITCODE -ne 0) { $problems.Add("cannot reach $remote to check its tags") }
elseif ($remoteTag) { $problems.Add("tag $tag already exists on $remote") }

# README names this release's zip.
if (-not (Select-String -LiteralPath (Join-Path $repo 'README.md') -SimpleMatch "NightwatchCoop-$tag-win64.zip" -Quiet)) {
    $problems.Add("README.md does not mention NightwatchCoop-$tag-win64.zip")
}

# Privacy scan of the package (see tools\privacy-scan.py).
if (-not (Test-Path -LiteralPath $patternsFile)) {
    $problems.Add("no privacy patterns: create $patternsFile (one per line)")
} elseif (Test-Path -LiteralPath $Package) {
    $patterns = @(Get-Content -LiteralPath $patternsFile | Where-Object { $_.Trim() })
    & python (Join-Path $PSScriptRoot 'privacy-scan.py') $Package @patterns
    if ($LASTEXITCODE -ne 0) { $problems.Add('privacy scan found new hits (above)') }
}

if ($problems.Count) {
    $problems | ForEach-Object { Write-Output "  ! $_" }
    throw "release $tag not published: $($problems.Count) problem(s)"
}
Write-Output "checks passed for $tag ($title)"

$steps = @(
    @('git', '-C', $repo, 'tag', '-a', $tag, '-m', $tag),
    @('git', '-C', $repo, 'push', $remote, "${branch}:main"),
    @('git', '-C', $repo, 'push', $remote, $tag),
    @($gh, 'release', 'create', $tag, '--repo', $repoName, '--prerelease', '--title', $title, '--notes-file', $Notes, $zip, $exe, $manifest)
)
foreach ($step in $steps) {
    if ($DryRun) {
        Write-Output ("would run: " + ($step -join ' '))
    } else {
        & $step[0] $step[1..($step.Count - 1)]
        if ($LASTEXITCODE -ne 0) { throw "failed: $($step -join ' ')" }
    }
}
if ($DryRun) { Write-Output 'dry run: nothing was tagged, pushed or published' }
