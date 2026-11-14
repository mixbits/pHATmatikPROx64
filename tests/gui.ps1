# On-screen tests for the installed product. Each one drives the plug-in's own window with real
# mouse and keyboard input, once with the original plug-in loaded directly by the 32-bit test
# host and once through the bridge by the 64-bit test host, and compares what came out.
#
#   .\tests\gui.ps1             the installed bridge
#   .\tests\gui.ps1 -UseBuild   the bridge in build\ instead
#
# KEEP YOUR HANDS OFF THE MOUSE AND KEYBOARD WHILE IT RUNS (about four minutes). The tests move
# the real pointer, hold Alt and Ctrl, and answer the plug-in's file dialogs; your own input
# collides with theirs and makes tests fail. The plug-in window opens on top of everything.
#
# Needs the plug-in authorized, or in a trial session where it makes sound.
param([switch]$UseBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$direct = "${env:ProgramFiles(x86)}\Steinberg\VstPlugins\iZotope pHATmatik PRO.dll"
$reg = Get-ItemProperty 'HKLM:\SOFTWARE\pHATmatik PRO Bridge' -ErrorAction SilentlyContinue
if (-not $reg) { $reg = Get-ItemProperty 'HKCU:\SOFTWARE\pHATmatik PRO Bridge' -ErrorAction SilentlyContinue }
if (-not $reg -or -not (Test-Path $direct)) { throw 'pHATmatik PRO and the bridge must be installed first.' }
$bridged = Join-Path $reg.Vst64Dir 'iZotope pHATmatik PRO.dll'
if ($UseBuild) {
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
$out = Join-Path $root 'build\test\gui'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null

# Positions inside the plug-in's 640x450 window.
$midiExport = '150,14'; $playPreview = '173,14'; $sliceButton = '40,14'; $browserButton = '18,13'; $masterAmpKnob = '417,297'
$hostStrip = '300,468'   # the test host's own area below the plug-in, which accepts drops

# The plug-in's browser shows seven rows and scrolls only by dragging its scrollbar. Works out
# the clicks that open $name inside $dir, starting at $time milliseconds.
function Open-Folder([string]$dir, [string]$name, [int]$time) {
    $n = @(Get-ChildItem $dir -Directory -Force | ForEach-Object { $_.Name.ToLowerInvariant() })
    [Array]::Sort($n, [StringComparer]::Ordinal)
    $i = [Array]::IndexOf($n, $name.ToLowerInvariant())
    if ($i -lt 0) { throw "$name not found in $dir" }
    if ($i -le 6) { return "$($time + 1600):click:700,$(41 + 15 * $i);" }
    return "$($time):drag:838,80,838,175;$($time + 2200):click:700,$(41 + 15 * ($i - ($n.Count - 7)));"
}

function Run([string]$exe, [string]$plugin, [string[]]$extra) {
    $common = @('--blocks', '0', '--timeout', '120', '--click', 'Continue,Demo,OK', '--topmost', '--dpiaware')
    return (& $exe $plugin @common @extra 2>&1 | ForEach-Object { "$_" })
}
function Pick($lines, [string]$pattern) { return (@($lines | Where-Object { $_ -match $pattern }) -join ' | ') }
function Same-File([string]$a, [string]$b) { return (Test-Path $a) -and (Test-Path $b) -and ((Get-Item $a).Length -gt 0) -and ((Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash) }

$failed = 0
function Report([string]$label, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Host "PASS  $label" } else { $script:failed++; Write-Host "FAIL  $label" }
    Write-Host "        $detail"
}

$created = -not (Test-Path $loopDir)
New-Item -ItemType Directory -Force $loopDir | Out-Null
python -I (Join-Path $PSScriptRoot 'make_loop.py') (Join-Path $loopDir 'loop.wav') | Out-Null
python -I (Join-Path $PSScriptRoot 'make_aiff.py') (Join-Path $loopDir 'loop.wav') (Join-Path $loopDir 'a_loop.aif') | Out-Null
Remove-Item (Join-Path $loopDir 'b_phat.wav') -ErrorAction SilentlyContinue
try {
    $load = @('--loadchunk', $state)

    # 1. MIDI export through the plug-in's "Save" dialog.
    Run $host32 $direct ($load + @('--editor', '9', '--actions', "1300:filedlg:$out\export_direct.mid;1500:click:$midiExport")) | Out-Null
    Run $host64 $bridged ($load + @('--editor', '9', '--actions', "1300:filedlg:$out\export_bridged.mid;1500:click:$midiExport")) | Out-Null
    Report 'MIDI export button (file dialog)' (Same-File "$out\export_direct.mid" "$out\export_bridged.mid") "both wrote the same $((Get-Item "$out\export_bridged.mid" -ErrorAction SilentlyContinue).Length)-byte MIDI file"

    # 2. MIDI dragged out of the plug-in onto the host (Alt + drag from the export button).
    $a = Run $host32 $direct ($load + @('--editor', '9', '--droptarget', '--dropsave', "$out\dragmidi_direct", '--actions', "1500:altdrag:$midiExport,$hostStrip"))
    $b = Run $host64 $bridged ($load + @('--editor', '9', '--droptarget', '--dropsave', "$out\dragmidi_bridged", '--actions', "1500:altdrag:$midiExport,$hostStrip"))
    Report 'MIDI dragged from the plug-in to the host' ((Same-File "$out\dragmidi_direct_0.mid" "$out\dragmidi_bridged_0.mid") -and ($b -match 'dropped on the host window: 1 file')) (Pick $b 'dropped on the host window|TIMEOUT')

    # 3. Slices dragged out (click one, Ctrl+click another, Alt + drag from the waveform).
    $acts = "1500:click:100,130;2700:ctrlclick:200,130;3900:altdrag:150,130,$hostStrip"
    $a = Run $host32 $direct ($load + @('--editor', '9', '--droptarget', '--actions', $acts))
    $b = Run $host64 $bridged ($load + @('--editor', '9', '--droptarget', '--actions', $acts))
    $da = Pick $a 'droptarget:   file|dropped on the host'; $db = Pick $b 'droptarget:   file|dropped on the host'
    Report 'slices dragged from the plug-in to the host' (($da -eq $db) -and ($db -match '2 file')) $db

    # 4. Play-preview button with no MIDI coming in.
    $quiet = @('--audiothread', '--notebase', '0', '--notespan', '1')
    $a = Run $host32 $direct ($load + $quiet + @('--editor', '7', '--actions', "1500:click:$playPreview"))
    $b = Run $host64 $bridged ($load + $quiet + @('--editor', '7', '--actions', "1500:click:$playPreview"))
    $pa = [regex]::Match((Pick $a 'audio thread'), 'peak ([\d.]+)').Groups[1].Value; $pb = [regex]::Match((Pick $b 'audio thread'), 'peak ([\d.]+)').Groups[1].Value
    Report 'play-preview button makes sound' (($pa -eq $pb) -and ([double]$pb -gt 0)) "loudest sample: direct $pa, bridged $pb"

    # 5. Knob dragged with the mouse while audio runs: the host must be told, on its main thread.
    $a = Run $host32 $direct ($load + @('--editor', '5', '--audiothread', '--dragtest', $masterAmpKnob))
    $b = Run $host64 $bridged ($load + @('--editor', '5', '--audiothread', '--dragtest', $masterAmpKnob))
    $ka = Pick $a 'dragtest after'; $kb = Pick $b 'dragtest after'
    Report 'knob drag reaches the host' (($ka -eq $kb) -and ($kb -match 'beginEdit=1 automate=\d+ endEdit=1') -and ($kb -match 'off the main thread: 0')) ($kb -replace '^dragtest after: ', '')

    # 6. Browser button: the plug-in resizes its own window.
    $a = Run $host32 $direct @('--editor', '5', '--realclick', $browserButton)
    $b = Run $host64 $bridged @('--editor', '5', '--realclick', $browserButton)
    $ra = Pick $a '^realclick'; $rb = Pick $b '^realclick'
    Report 'browser opens and the window resizes' (($ra -eq $rb) -and ($rb -match 'effEditGetRect 850x450')) ($rb -replace '^realclick [\d,]+: ', '')

    # 7. Loading an AIFF file from the browser, slicing it, and playing the slices.
    $nav = "1500:click:$browserButton;3000:click:665,55;" + (Open-Folder 'D:\' 'Projects' 4600) + (Open-Folder 'D:\Projects' '!phatmatik_test' 8400)
    $play = @('--notebase', '48', '--notespan', '18', '--postblocks', '1500')
    $a = Run $host32 $direct ($play + @('--editor', '17', '--actions', "$($nav)11800:dbl:700,170;14200:click:$sliceButton"))
    $b = Run $host64 $bridged ($play + @('--editor', '17', '--actions', "$($nav)11800:dbl:700,170;14200:click:$sliceButton"))
    $la = Pick $a '^post-load'; $lb = Pick $b '^post-load'
    Report 'AIFF loaded from the browser, sliced, played' (($la -eq $lb) -and ($lb -notmatch 'peak=0\.000000') -and ($lb -match 'post-load chunk: \d{4}')) $lb

    # 8. Save as pHATfile, then load that pHATfile back from the browser. The plug-in writes it
    #    under the loop's own name in the folder its dialog opens in, whatever is typed, so each
    #    side's file is collected from where it landed.
    foreach ($side in @(@($host32, $direct, 'direct'), @($host64, $bridged, 'bridged'))) {
        $before = Get-Date
        Run $side[0] $side[1] ($load + @('--editor', '9', '--actions', "1300:filedlg:$out\phat.wav;1500:click:265,14")) | Out-Null
        $found = Get-ChildItem $out, (Join-Path $root 'build\test'), "$env:USERPROFILE\Desktop", "$env:USERPROFILE\Documents" -Filter 'loop.wav' -File -ErrorAction SilentlyContinue |
                 Where-Object {
                     # Only a file this run has just written, and only if it really is a pHATfile.
                     $_.LastWriteTime -ge $before -and $_.Length -gt 1000000 -and
                     [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($_.FullName), 36, 4) -eq 'pHAT'
                 } | Select-Object -First 1
        if ($found) { Move-Item $found.FullName "$out\phat_$($side[2]).wav" -Force }
    }
    python -I (Join-Path $PSScriptRoot 'make_loop.py') (Join-Path $root 'build\test\loop.wav') | Out-Null   # in case the save landed on it
    Report 'Save as pHATfile' (Same-File "$out\phat_direct.wav" "$out\phat_bridged.wav") "both wrote the same $((Get-Item "$out\phat_bridged.wav" -ErrorAction SilentlyContinue).Length)-byte file"
    if (Test-Path "$out\phat_bridged.wav") {
        Copy-Item "$out\phat_bridged.wav" (Join-Path $loopDir 'b_phat.wav') -Force
        $a = Run $host32 $direct ($play + @('--editor', '15', '--actions', "$($nav)11800:dbl:700,185"))
        $b = Run $host64 $bridged ($play + @('--editor', '15', '--actions', "$($nav)11800:dbl:700,185"))
        $la = Pick $a '^post-load'; $lb = Pick $b '^post-load'
        Report 'pHATfile loaded back, slices intact' (($la -eq $lb) -and ($lb -notmatch 'peak=0\.000000')) $lb
    }
} finally {
    if ($created) { Remove-Item $loopDir -Recurse -Force }
}
if ($failed) { Write-Host "$failed on-screen test(s) FAILED"; exit 1 }
Write-Host 'All on-screen tests passed'
