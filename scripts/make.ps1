# The deployment command is build.ps1 in the project folder. This name calls it.
param([string]$Installer, [string]$OutputDir)
$build = Join-Path (Split-Path $PSScriptRoot -Parent) 'build.ps1'
if ($Installer -and $OutputDir) {
    & $build -Installer $Installer -OutputDir $OutputDir
} elseif ($Installer) {
    & $build -Installer $Installer
} elseif ($OutputDir) {
    & $build -OutputDir $OutputDir
} else {
    & $build
}
if (-not $?) { exit 1 }
