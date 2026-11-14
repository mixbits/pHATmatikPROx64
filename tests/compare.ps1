# Runs the same test sequence against a 32-bit plug-in twice - loaded directly by the 32-bit test
# host, and loaded through the bridge by the 64-bit test host - and compares what the host saw.
#
#   .\tests\compare.ps1                 the validation plug-in, several block sizes and modes
#   .\tests\compare.ps1 -Phatmatik      the original pHATmatik PRO DLL from extracted\
#
# With -Phatmatik the plug-in's own start-up prompt is answered automatically: "Continue" during the
# trial, or "Demo" then "OK" once the trial has expired. Pass -Installed to test the copy that
# iZotope's installer put in Program Files (x86) instead of the one in extracted/.
param([switch]$Phatmatik, [switch]$Installed, [switch]$ShowOutput)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$host32 = Join-Path $root 'build\x86\testhost32.exe'
$host64 = Join-Path $root 'build\x64\testhost64.exe'
$bridge = Join-Path $root 'build\x64\iZotope pHATmatik PRO.dll'
$helper = Join-Path $root 'build\x86\pHATmatikBridgeHost32.exe'
$stage = Join-Path $root 'build\test\stage'
New-Item -ItemType Directory -Force $stage | Out-Null

function Stage([string]$name, [string]$plugin32) {
    # A copy of the bridge with a sidecar .ini that points it at the plug-in under test.
    $dll = Join-Path $stage "$name.dll"
    Copy-Item $bridge $dll -Force
    "[Bridge]`r`nHost32=$helper`r`nPlugin32=$plugin32`r`nDebug=0" | Set-Content -Encoding ASCII (Join-Path $stage "$name.ini")
    return $dll
}

function Normalize($lines) {
    $lines | ForEach-Object { "$_" } |
        Where-Object { $_ -notmatch '^testhost \(|^exit code|\[watchdog\]|\[windows:' -and $_ -notmatch '^\s+(top|child) ' } |
        ForEach-Object { $_ -replace 'loaded in \d+ ms', 'loaded' -replace 'returned after \d+ ms', 'returned' -replace '[\d.]+ us/block, ', '' }
}

$failed = 0
function Compare-Run([string]$label, [string]$plugin32, [string]$bridged, [string[]]$extra, [switch]$Split) {
    $a = & $host32 $plugin32 @extra 2>&1
    $codeA = $LASTEXITCODE
    $b = & $host64 $bridged @extra 2>&1
    $codeB = $LASTEXITCODE
    $na = @(Normalize $a); $nb = @(Normalize $b)
    if ($Split) {
        # The bridge passes a block this large to the plug-in in several parts, so the plug-in
        # makes its per-call host callbacks more often. Everything else must still match.
        $na = $na -replace ' op\d+ x\d+', ''; $nb = $nb -replace ' op\d+ x\d+', ''
    }
    $diff = Compare-Object $na $nb
    $render = ($nb | Where-Object { $_ -match '^render:' }) -replace '^render: ', ''
    $timeA = [regex]::Match(($a -join "`n"), '([\d.]+) us/block').Groups[1].Value
    $timeB = [regex]::Match(($b -join "`n"), '([\d.]+) us/block').Groups[1].Value
    if ($codeA -eq 0 -and $codeB -eq 0 -and -not $diff) {
        Write-Host ("PASS  {0,-34} {1} lines identical; {2}; direct {3} us/block, bridged {4} us/block" -f $label, $nb.Count, $render, $timeA, $timeB)
    } else {
        $script:failed++
        Write-Host ("FAIL  {0}  (exit codes {1}/{2})" -f $label, $codeA, $codeB)
        $diff | ForEach-Object { Write-Host ("      {0} {1}" -f $_.SideIndicator, $_.InputObject) }
    }
    if ($ShowOutput) { $b | ForEach-Object { Write-Host "      | $_" } }
}

if ($Phatmatik) {
    $orig = (Get-ChildItem -Recurse (Join-Path $root 'extracted') -Filter 'iZotope pHATmatik PRO.dll').FullName
    if ($Installed) { $orig = "${env:ProgramFiles(x86)}\Steinberg\VstPlugins\iZotope pHATmatik PRO.dll" }
    if (-not (Test-Path $orig)) { throw "Plug-in not found: $orig" }
    if ($Installed) {
        # The bridge exactly as the installer left it, configured through the registry.
        $dll = Join-Path (Get-ItemProperty 'HKLM:\SOFTWARE\pHATmatik PRO Bridge').Vst64Dir 'iZotope pHATmatik PRO.dll'
        Write-Host "direct:  $orig"; Write-Host "bridged: $dll"
    } else {
        $dll = Stage 'phatmatik' $orig
    }
    $common = @('--timeout', '90', '--click', 'Continue,Demo,OK', '--params')
    Compare-Run 'pHATmatik: 300 blocks of 512' $orig $dll ($common + @('--blocks', '300'))
    Compare-Run 'pHATmatik: 64-sample blocks' $orig $dll ($common + @('--blocks', '300', '--blocksize', '64'))
    Compare-Run 'pHATmatik: automation while processing' $orig $dll ($common + @('--blocks', '300', '--automate'))
} else {
    $plug = Join-Path $root 'build\x86\testplug32.dll'
    $dll = Stage 'testplug' $plug
    $common = @('--timeout', '60', '--params', '--sysex', '--automate')
    Compare-Run 'blocks of 512' $plug $dll ($common + @('--blocks', '600'))
    Compare-Run 'blocks of 1' $plug $dll ($common + @('--blocks', '3000', '--blocksize', '1'))
    Compare-Run 'blocks of 64' $plug $dll ($common + @('--blocks', '2000', '--blocksize', '64'))
    Compare-Run 'blocks of 4096' $plug $dll ($common + @('--blocks', '200', '--blocksize', '4096'))
    Compare-Run 'blocks of 16384' $plug $dll ($common + @('--blocks', '60', '--blocksize', '16384'))
    Compare-Run 'blocks of 40000 (split by bridge)' $plug $dll ($common + @('--blocks', '40', '--blocksize', '40000')) -Split
    Compare-Run 'accumulating process()' $plug $dll ($common + @('--blocks', '300', '--accumulate'))
}
if ($failed) { Write-Host "$failed comparison(s) FAILED"; exit 1 }
Write-Host 'All comparisons passed'
