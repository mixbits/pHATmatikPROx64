# Unpacks the owner's copy of iZotope's installer into extracted\.
#   dist\iZotope_pHATmatik_PRO_v1_52.exe is an input. This script does not create it
#   and does not modify it. extracted\ is gitignored.
#   .\scripts\unpack-installer.ps1
#   -OutputDir   write somewhere else (the default is extracted\ at the repo root)
# innoextract 1.9 is included at tools\innoextract\innoextract.exe. A copy on PATH is used if that file is absent.
param([string]$OutputDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$installer = Join-Path $root 'dist\iZotope_pHATmatik_PRO_v1_52.exe'
if (-not (Test-Path -LiteralPath $installer)) {
    throw "iZotope's original installer is missing from dist\. Run .\build.ps1 -Installer `"C:\path\to\iZotope_pHATmatik_PRO_v1_52.exe`" with your own copy."
}
$bundled = Join-Path $root 'tools\innoextract\innoextract.exe'
$onPath = Get-Command innoextract -ErrorAction SilentlyContinue
$innoextract = if (Test-Path -LiteralPath $bundled) { $bundled } elseif ($onPath) { $onPath.Source } else { $null }
if (-not $innoextract) {
    throw "innoextract 1.9 was not found at tools\innoextract\innoextract.exe or on PATH. Restore that folder, or install it with: winget install dscharrer.innoextract"
}
if (-not $OutputDir) { $OutputDir = Join-Path $root 'extracted' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# innoextract warns that the installer's Japanese strings are not Windows-1252.
# PowerShell 5.1 reports that stderr line as an error. The files still extract;
# the exit code is what decides.
$ErrorActionPreference = 'Continue'
& $innoextract -e -d $OutputDir -q $installer 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "innoextract failed with exit code $LASTEXITCODE." }

$dll = Get-ChildItem -LiteralPath $OutputDir -Recurse -Filter 'iZotope pHATmatik PRO.dll' | Select-Object -First 1
$license = Join-Path $OutputDir 'app\License_en.rtf'
if (-not $dll -or -not (Test-Path -LiteralPath $license)) {
    throw "Unpack finished but the plug-in DLL or app\License_en.rtf is missing from $OutputDir."
}
Write-Host ("Unpacked into {0}" -f $OutputDir)
Write-Host $dll.FullName
