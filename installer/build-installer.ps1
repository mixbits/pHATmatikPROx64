# Builds the one-file installers.
#   pHATmatik_PRO_v1_52_x64_Setup.exe             (project root) iZotope's original installer + the bridge
#   dist\pHATmatik_PRO_x64_Bridge_Only_Setup.exe  the bridge only (contains no iZotope files)
# iZotope's original installer is read from dist\iZotope_pHATmatik_PRO_v1_52.exe. It is an input,
# not something this script produces: do not delete it when clearing out dist\.
# Needs Inno Setup 6. It is looked for in tools\InnoSetup, then in Program Files.
#   -SkipBuild   package the binaries already in build\ instead of rebuilding them
#   -OutputDir   write both installers to this folder instead (to check the script without
#                replacing the released files)
param([switch]$SkipBuild, [string]$OutputDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$iscc = @(
    (Join-Path $root 'tools\InnoSetup\ISCC.exe'),
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found. Install it, or unpack it into tools\InnoSetup.' }
if (-not (Test-Path (Join-Path $root 'dist\iZotope_pHATmatik_PRO_v1_52.exe'))) { throw "iZotope's original installer is missing from dist\." }
# -CompileOnly: build.ps1's default path calls this script. Without the switch the two would call each other.
if (-not $SkipBuild) { & (Join-Path $root 'build.ps1') -CompileOnly }
$iss = Join-Path $PSScriptRoot 'phatmatik_bridge.iss'
$override = @()
if ($OutputDir) { $override = @("/O$OutputDir") }
& $iscc /Qp @override $iss
if ($LASTEXITCODE -ne 0) { throw 'Compiling the bundled installer failed.' }
& $iscc /Qp @override /DBridgeOnly $iss
if ($LASTEXITCODE -ne 0) { throw 'Compiling the bridge-only installer failed.' }
$outputs = if ($OutputDir) { Get-ChildItem $OutputDir -Filter '*Setup.exe' }
           else { @(Get-Item (Join-Path $root 'pHATmatik_PRO_v1_52_x64_Setup.exe')) + @(Get-ChildItem (Join-Path $root 'dist') -Filter '*Setup.exe') }
$outputs | ForEach-Object { '{0}  {1:N0} bytes  SHA-256 {2}' -f $_.FullName, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
