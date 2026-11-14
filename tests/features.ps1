# Feature tests for the installed product: each run is done twice, with the original plug-in
# loaded directly by the 32-bit test host and through the bridge by the 64-bit test host, from
# the same saved state (a sliced drum loop), and everything the host observes must match.
#
#   .\tests\features.ps1            everything that needs no mouse
#   .\tests\features.ps1 -Soak      adds a long run (about 39 minutes of audio per side)
#
# Needs the plug-in authorized, or in a trial session where it makes sound. The editor-cycles
# test opens a window briefly; nothing else appears on screen.
#   .\tests\features.ps1 -UseBuild  tests the bridge in build\ instead of the installed one
param([switch]$Soak, [switch]$UseBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$direct = "${env:ProgramFiles(x86)}\Steinberg\VstPlugins\iZotope pHATmatik PRO.dll"
$reg = Get-ItemProperty 'HKLM:\SOFTWARE\pHATmatik PRO Bridge' -ErrorAction SilentlyContinue
if (-not $reg) { $reg = Get-ItemProperty 'HKCU:\SOFTWARE\pHATmatik PRO Bridge' -ErrorAction SilentlyContinue }
if (-not $reg -or -not (Test-Path $direct)) { throw 'pHATmatik PRO and the bridge must be installed first.' }
$bridged = Join-Path $reg.Vst64Dir 'iZotope pHATmatik PRO.dll'
if ($UseBuild) {
    # A copy of the freshly built bridge, pointed at the freshly built helper and the installed
    # original plug-in by a sidecar .ini.
    $stage = Join-Path $root 'build\test\stage'
    New-Item -ItemType Directory -Force $stage | Out-Null
    $bridged = Join-Path $stage 'newbridge.dll'
    Copy-Item (Join-Path $root 'build\x64\iZotope pHATmatik PRO.dll') $bridged -Force
    "[Bridge]`r`nHost32=$(Join-Path $root 'build\x86\pHATmatikBridgeHost32.exe')`r`nPlugin32=$direct`r`nDebug=0" | Set-Content -Encoding ASCII (Join-Path $stage 'newbridge.ini')
}
Write-Host "bridge under test: $bridged"
$host32 = Join-Path $root 'build\x86\testhost32.exe'
$host64 = Join-Path $root 'build\x64\testhost64.exe'
$state = Join-Path $PSScriptRoot 'data\loop_state.bin'
$loopDir = 'D:\Projects\!phatmatik_test'   # the path recorded inside loop_state.bin

function Normalize($lines) {
    $lines | ForEach-Object { "$_" } |
        Where-Object { $_ -notmatch '^testhost \(|^exit code|\[watchdog\]|\[windows:|window icon|cycle timing' -and $_ -notmatch '^\s+(top|child) ' } |
        ForEach-Object { $_ -replace 'loaded in \d+ ms', 'loaded' -replace 'returned after \d+ ms', 'returned' -replace '[\d.]+ us/block, ', '' }
}

$failed = 0
function Compare-Run([string]$label, [string]$show, [string[]]$extra) {
    # The loop's 18 slices answer to MIDI notes 48 to 65; the test host plays those in turn.
    $common = @('--timeout', '900', '--click', 'Continue,Demo,OK', '--loadchunk', $state, '--notebase', '48', '--notespan', '18')
    $a = & $host32 $direct @common @extra 2>&1; $codeA = $LASTEXITCODE
    $b = & $host64 $bridged @common @extra 2>&1; $codeB = $LASTEXITCODE
    $na = @(Normalize $a); $nb = @(Normalize $b)
    $diff = Compare-Object $na $nb
    $line = "$($nb | Where-Object { $_ -match $show } | Select-Object -First 1)"
    $silent = $line -match 'peak=0\.000000'
    if ($codeA -eq 0 -and $codeB -eq 0 -and -not $diff -and -not $silent) {
        Write-Host ("PASS  {0,-38} {1} lines identical" -f $label, $nb.Count)
        Write-Host "        $line"
    } else {
        $script:failed++
        Write-Host ("FAIL  {0}  (exit codes {1}/{2}{3})" -f $label, $codeA, $codeB, $(if ($silent) { ', output is silent' }))
        Write-Host "        $line"
        $diff | Select-Object -First 12 | ForEach-Object { Write-Host ("      {0} {1}" -f $_.SideIndicator, $_.InputObject) }
    }
}

$created = -not (Test-Path $loopDir)
New-Item -ItemType Directory -Force $loopDir | Out-Null
python -I (Join-Path $PSScriptRoot 'make_loop.py') (Join-Path $loopDir 'loop.wav') | Out-Null
try {
    Compare-Run 'saved state, 2000 blocks' '^render:' @('--blocks', '2000', '--params')
    Compare-Run 'all 48 parameters swept with sound' '^sweep:' @('--blocks', '100', '--sweep')
    Compare-Run '16 programs walked' '^programs:' @('--blocks', '100', '--programs')
    Compare-Run '48 kHz, 256-sample blocks, 97 bpm' '^render:' @('--blocks', '3000', '--samplerate', '48000', '--blocksize', '256', '--tempo', '97')
    Compare-Run '96 kHz, 1024-sample blocks, 140 bpm' '^render:' @('--blocks', '1500', '--samplerate', '96000', '--blocksize', '1024', '--tempo', '140')
    Compare-Run '22.05 kHz, 64-sample blocks, 174 bpm' '^render:' @('--blocks', '4000', '--samplerate', '22050', '--blocksize', '64', '--tempo', '174')
    Compare-Run '8192-sample blocks' '^render:' @('--blocks', '300', '--blocksize', '8192')
    Compare-Run 'parameter changes while processing' '^render:' @('--blocks', '2000', '--automate')
    Compare-Run '6 instances at once, each with the loop' 'instances rendered' @('--blocks', '1500', '--instances', '6')
    Compare-Run 'editor opened and closed 25 times' '^editor cycles:' @('--blocks', '50', '--editorcycles', '25')
    if ($Soak) { Compare-Run 'soak: 200,000 blocks' '^render:' @('--blocks', '200000', '--automate') }
} finally {
    if ($created) { Remove-Item $loopDir -Recurse -Force }
}
Start-Sleep -Milliseconds 500
$left = @(Get-Process pHATmatikBridgeHost32 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -eq '' -and $_.Parent -eq $null }).Count
if ($failed) { Write-Host "$failed feature test(s) FAILED"; exit 1 }
Write-Host 'All feature tests passed'
