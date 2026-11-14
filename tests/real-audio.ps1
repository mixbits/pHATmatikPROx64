# Real pHATmatik PRO sound, loaded directly and through the bridge, compared sample for sample.
#
# Needs the product installed (iZotope's installer and the bridge) and a trial or authorization
# under which the plug-in makes sound. Takes about two minutes and shows the plug-in's trial
# prompt twice; the test host presses "Continue".
#
# How it works: tests\data\loop_state.bin is the plug-in's own saved state after a drum loop was
# loaded from its browser and sliced. The state names the loop by path, so the loop is recreated
# at that path, the state is restored in a fresh instance - once in the 32-bit test host, once
# through the bridge in the 64-bit test host - and 2000 blocks are rendered while MIDI notes
# trigger the slices.
#
# The two runs are a little over a minute apart on purpose. During its trial the plug-in shows
# its prompt at most about once a minute, and an instance loaded without the prompt stays silent.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$direct = "${env:ProgramFiles(x86)}\Steinberg\VstPlugins\iZotope pHATmatik PRO.dll"
$reg = Get-ItemProperty 'HKLM:\SOFTWARE\pHATmatik PRO Bridge' -ErrorAction SilentlyContinue
if (-not $reg) { $reg = Get-ItemProperty 'HKCU:\SOFTWARE\pHATmatik PRO Bridge' -ErrorAction SilentlyContinue }
if (-not $reg -or -not (Test-Path $direct)) { throw 'pHATmatik PRO and the bridge must be installed first.' }
$bridged = Join-Path $reg.Vst64Dir 'iZotope pHATmatik PRO.dll'
$state = Join-Path $PSScriptRoot 'data\loop_state.bin'
$loopDir = 'D:\Projects\!phatmatik_test'   # the path recorded inside loop_state.bin
$out = Join-Path $root 'build\test'
New-Item -ItemType Directory -Force $out | Out-Null

$created = -not (Test-Path $loopDir)
New-Item -ItemType Directory -Force $loopDir | Out-Null
python -I (Join-Path $PSScriptRoot 'make_loop.py') (Join-Path $loopDir 'loop.wav') | Out-Null
try {
    $args32 = @($direct, '--blocks', '2000', '--notespan', '48', '--timeout', '120', '--click', 'Continue,Demo,OK', '--loadchunk', $state, '--wav', "$out\real_direct32.wav")
    $args64 = @($bridged, '--blocks', '2000', '--notespan', '48', '--timeout', '120', '--click', 'Continue,Demo,OK', '--loadchunk', $state, '--wav', "$out\real_bridged64.wav")
    Write-Host 'Waiting 75 s so that the plug-in shows its trial prompt...'
    Start-Sleep -Seconds 75
    $a = & (Join-Path $root 'build\x86\testhost32.exe') @args32 2>&1
    Write-Host 'Waiting 75 s again...'
    Start-Sleep -Seconds 75
    $b = & (Join-Path $root 'build\x64\testhost64.exe') @args64 2>&1
} finally {
    if ($created) { Remove-Item $loopDir -Recurse -Force }
}
$ra = "$($a | Where-Object { $_ -match '^render:' })" -replace '[\d.]+ us/block, ', ''
$rb = "$($b | Where-Object { $_ -match '^render:' })" -replace '[\d.]+ us/block, ', ''
Write-Host "direct : $ra"
Write-Host "bridged: $rb"
$same = (Get-FileHash "$out\real_direct32.wav").Hash -eq (Get-FileHash "$out\real_bridged64.wav").Hash
$audible = $ra -notmatch 'peak=0\.000000'
if ($same -and $audible -and $ra -eq $rb) {
    Write-Host 'PASS  the rendered audio is identical, sample for sample, and not silent'
    Write-Host "      listen to it: $out\real_bridged64.wav"
} elseif ($same) {
    Write-Host 'INCONCLUSIVE  both renders are silent: the plug-in did not make sound (trial prompt not shown, trial expired, or loop not found)'
    exit 2
} else {
    Write-Host 'FAIL  the direct and bridged renders differ'
    exit 1
}
