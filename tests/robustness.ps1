# Failure and load tests for the bridge, using the validation plug-in (no dialogs appear).
#   .\tests\robustness.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$host32 = Join-Path $root 'build\x86\testhost32.exe'
$host64 = Join-Path $root 'build\x64\testhost64.exe'
$bridge = Join-Path $root 'build\x64\iZotope pHATmatik PRO.dll'
$helper = Join-Path $root 'build\x86\pHATmatikBridgeHost32.exe'
$plug = Join-Path $root 'build\x86\testplug32.dll'
$stage = Join-Path $root 'build\test\stage'
$log = Join-Path $env:TEMP 'pHATmatikBridge.log'
New-Item -ItemType Directory -Force $stage | Out-Null

function Stage([string]$name, [string]$plugin32) {
    $dll = Join-Path $stage "$name.dll"
    Copy-Item $bridge $dll -Force
    "[Bridge]`r`nHost32=$helper`r`nPlugin32=$plugin32`r`nDebug=0" | Set-Content -Encoding ASCII (Join-Path $stage "$name.ini")
    return $dll
}
function Helpers { @(Get-Process pHATmatikBridgeHost32 -ErrorAction SilentlyContinue).Count }

$failed = 0
function Check([string]$label, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Host "PASS  $label - $detail" } else { $script:failed++; Write-Host "FAIL  $label - $detail" }
}

$dll = Stage 'testplug' $plug
$before = Helpers

# 1. The helper process dies while the plug-in is in use.
$out = & $host64 $dll --blocks 100 --timeout 30 --killhelper 2>&1
$line = "$($out | Where-Object { $_ -match '^killed' })"
Check 'helper killed mid-session' ($LASTEXITCODE -eq 0 -and $line -match 'killed 1 helper' -and $line -match 'peak=0\.000000' -and ($out -match 'effClose after helper death done')) $line

# 2. The host ends without closing the plug-in.
& $host64 $dll --blocks 50 --timeout 30 --abandon 2>&1 | Out-Null
Start-Sleep -Milliseconds 1500
Check 'host ended without effClose' ((Helpers) -eq $before) "helper processes left running: $((Helpers) - $before)"

# 3. Several instances, each processed on its own thread at the same time.
$a = & $host32 $plug --instances 8 --blocks 400 --timeout 60 2>&1 | Where-Object { $_ -match 'instances rendered' }
$b = & $host64 $dll --instances 8 --blocks 400 --timeout 60 2>&1 | Where-Object { $_ -match 'instances rendered' }
Check '8 simultaneous instances' ("$a" -eq "$b" -and "$b" -match 'all instances identical') "$b"
Start-Sleep -Milliseconds 500
Check 'helpers gone after closing 8 instances' ((Helpers) -eq $before) "helper processes left running: $((Helpers) - $before)"

# 4. The original plug-in is missing.
$missing = Stage 'missing' (Join-Path $root 'build\x86\does-not-exist.dll')
$out = & $host64 $missing --timeout 30 2>&1
$last = Get-Content $log -Tail 1
Check 'original plug-in missing' ($LASTEXITCODE -eq 3 -and $last -match 'was not found') "host reports load failure; log: $($last -replace '^.*?\] ', '')"

# 5. The configured plug-in is not a 32-bit DLL.
$wrong = Stage 'wrongarch' $bridge
$out = & $host64 $wrong --timeout 30 2>&1
$last = Get-Content $log -Tail 1
Check 'configured DLL is 64-bit' ($LASTEXITCODE -eq 3 -and $last -match 'error 193') "host reports load failure; log: $($last -replace '^.*?\] ', '')"
Start-Sleep -Milliseconds 500
Check 'no helper left behind' ((Helpers) -eq $before) "helper processes left running: $((Helpers) - $before)"

if ($failed) { Write-Host "$failed check(s) FAILED"; exit 1 }
Write-Host 'All robustness checks passed'
