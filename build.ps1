# Builds the 64-bit setup from your copy of iZotope's installer.
#
#   .\build.ps1 -Installer "C:\path\to\iZotope_pHATmatik_PRO_v1_52.exe"
#   .\build.ps1
#       uses dist\iZotope_pHATmatik_PRO_v1_52.exe if it is already there
#
# Creates dist\, extracted\, and build\ when a clone does not have them, checks
# the installer, unpacks it, compiles the bridge, and writes
# pHATmatik_PRO_v1_52_x64_Setup.exe in this folder. Your original file is not modified.
#
#   -OutputDir    write the setup programs into this folder instead
#   -CompileOnly  compile into build\ and do not build the setup program
#   -Target       with -CompileOnly: all, bridge, host, or testhost
#   -Debug        with -CompileOnly: unoptimized build
param(
    [ValidateSet('all', 'bridge', 'host', 'testhost')] [string]$Target = 'all',
    [switch]$Debug,
    [switch]$CompileOnly,
    [string]$Installer,
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$destName = 'iZotope_pHATmatik_PRO_v1_52.exe'
$setupName = 'pHATmatik_PRO_v1_52_x64_Setup.exe'
$expectedHash = '9205FA07ABB6B5388B2DBA0D1C0638AA8E3918D062F5CAD99428467663BB6A9E'
$dest = Join-Path $root "dist\$destName"

function Fail([string]$message) {
    Write-Host ''
    Write-Host 'Stopped.'
    Write-Host $message
    exit 1
}

function Resolve-GivenPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return $path }
    return [IO.Path]::GetFullPath((Join-Path (Get-Location) $path))
}

function Compile {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ build tools not found.' }
    $devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'

    # /MT: no Visual C++ runtime needs to be installed on the user's machine.
    $common = '/nologo /std:c++17 /EHsc /W3 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /utf-8'
    if ($Debug) { $common += ' /Od /Zi' } else { $common += ' /O2 /DNDEBUG' }

    function Build([string]$arch, [string]$name, [string]$sources, [string]$output, [string]$extra, [string]$link, [string]$rcDefine, [string]$rcFile) {
        $outDir = Join-Path $root "build\$arch"
        $objDir = Join-Path $root "build\obj\$arch\$name"
        New-Item -ItemType Directory -Force $outDir, $objDir | Out-Null
        $vsArch = if ($arch -eq 'x64') { 'amd64' } else { 'x86' }
        $rc = ''
        $res = ''
        if ($rcDefine) {
            $res = "`"$objDir\version.res`""
            $rc = "rc /nologo /d $rcDefine /fo $res src\common\version.rc && "
        } elseif ($rcFile) {
            $res = "`"$objDir\resources.res`""
            $rc = "rc /nologo /I `"$root\build\obj`" /fo $res $rcFile && "
        }
        # A program that is running cannot be overwritten, but it can be renamed out of the way.
        $target = Join-Path $outDir $output
        if (Test-Path $target) {
            try { [IO.File]::Open($target, 'Open', 'ReadWrite', 'None').Close() }
            catch { Move-Item $target "$target.in-use-$(Get-Date -Format HHmmss)" -Force }
        }
        $cmd = "`"$devcmd`" -no_logo -arch=$vsArch -host_arch=amd64 >nul && cd /d `"$root`" && $rc" +
               "cl $common $extra /Fo`"$objDir\\`" /Fd`"$objDir\\`" $sources /Fe`"$outDir\$output`" /link /INCREMENTAL:NO $link $res"
        Write-Host "--- $arch $output"
        cmd /c $cmd
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $output" }
    }

    if ($Target -in 'all', 'testhost') {
        # The window and taskbar icon: the project's own drawing (tools\draw-app-icon.ps1), not
        # iZotope's logo or artwork.
        $hostRc = ''
        if (Test-Path (Join-Path $root 'images\app-icon.png')) {
            & (Join-Path $root 'tools\make-icon.ps1') -Source (Join-Path $root 'images\app-icon.png') -Output (Join-Path $root 'build\obj\app.ico')
            $hostRc = 'src\testhost\testhost.rc'
        }
        Build 'x86' 'testhost' 'src\testhost\testhost.cpp' 'testhost32.exe' '' '/SUBSYSTEM:CONSOLE' '' $hostRc
        Build 'x64' 'testhost' 'src\testhost\testhost.cpp' 'testhost64.exe' '' '/SUBSYSTEM:CONSOLE' '' $hostRc
        # The same program built without a console: double-click it to open the installed plug-in
        # in a window of its own, with sound.
        Build 'x64' 'standalone' 'src\testhost\testhost.cpp' 'pHATmatik PRO 64-bit.exe' '/DSTANDALONE_APP' '/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup' '' $hostRc
        Build 'x86' 'testplug' 'src\testplug\testplug.cpp' 'testplug32.dll' '/LD' ''
    }
    if ($Target -in 'all', 'host') {
        Build 'x86' 'host32' 'src\host32\host32.cpp' 'pHATmatikBridgeHost32.exe' '' '/SUBSYSTEM:WINDOWS /LARGEADDRESSAWARE' 'BRIDGE_HOST'
    }
    if ($Target -in 'all', 'bridge') {
        Build 'x64' 'bridge64' 'src\bridge64\bridge64.cpp' 'iZotope pHATmatik PRO.dll' '/LD' '/DEF:src\bridge64\bridge64.def' 'BRIDGE_DLL'
    }
    Write-Host 'Build OK'
}

$full = -not $CompileOnly -and -not $Debug -and $Target -eq 'all'
if (-not $full -and ($Installer -or $OutputDir)) {
    Fail 'Pass -Installer on its own. -CompileOnly, -Debug, and -Target compile into build\ and do not build the setup program.'
}
if (-not $full) {
    Compile
    return
}

# Git does not keep these folders. They are gitignored, and GitHub does not
# store an empty directory, so a clone does not have them until this script runs.
$folders = @(
    'dist',
    'extracted',
    'build',
    'build\x64',
    'build\x86',
    'build\obj',
    'build\test'
)
Write-Host 'Preparing folders'
$created = @()
foreach ($rel in $folders) {
    $path = Join-Path $root $rel
    if (-not (Test-Path -LiteralPath $path)) {
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        $created += $rel
    }
}
if ($created.Count) {
    Write-Host ("Created " + (($created | ForEach-Object { "$_\" }) -join ', '))
} else {
    Write-Host 'dist\, extracted\, and build\ are already in place.'
}

Write-Host ''
Write-Host '1/4  Checking your installer'
if ($Installer) {
    $source = Resolve-GivenPath $Installer
    if (-not (Test-Path -LiteralPath $source)) {
        Fail @"
No file at '$source'.
dist\, extracted\, and build\ are in place. Copy your installer to:
  $dest
Then run .\build.ps1
Or pass -Installer with the full path of iZotope_pHATmatik_PRO_v1_52.exe.
"@
    }
    $item = Get-Item -LiteralPath $source
    if ($item.PSIsContainer) {
        Fail "'$source' is a folder. Pass -Installer with the .exe file itself."
    }
} elseif (Test-Path -LiteralPath $dest) {
    $source = $dest
    Write-Host "Using $dest"
} else {
    Fail @"
iZotope's installer is not in this project. dist\ is ready for it.
Copy your own iZotope_pHATmatik_PRO_v1_52.exe to:
  $dest
Then run .\build.ps1
Or pass it directly:

  .\build.ps1 -Installer "C:\path\to\iZotope_pHATmatik_PRO_v1_52.exe"

The file has to be iZotope pHATmatik PRO v1.52. Its SHA-256 is $expectedHash.
"@
}

try {
    $bytes = [IO.File]::ReadAllBytes($source)
} catch {
    Fail "Could not read '$source'. Close anything that has the file open, then run this script again.`n$($_.Exception.Message)"
}
if ($bytes.Length -lt 2 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
    Fail "'$source' is not a Windows program. The file this script needs is iZotope_pHATmatik_PRO_v1_52.exe."
}
$hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ($hash -ne $expectedHash) {
    Fail @"
'$source' is not iZotope pHATmatik PRO v1.52.
This project builds the 64-bit setup from that installer only.
Expected SHA-256  $expectedHash
This file         $hash
"@
}

$sameFile = [IO.Path]::GetFullPath($source) -eq [IO.Path]::GetFullPath($dest)
if (-not $sameFile) {
    try {
        Copy-Item -LiteralPath $source -Destination $dest -Force
    } catch {
        Fail "Could not copy the installer to '$dest'.`n$($_.Exception.Message)"
    }
    $copied = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    if ($copied -ne $expectedHash) {
        Fail "The copy at '$dest' does not match the file you gave. Delete that copy and run .\build.ps1 again."
    }
    Write-Host "Copied to $dest"
}

Write-Host ''
Write-Host '2/4  Unpacking into extracted\'
try {
    & (Join-Path $root 'scripts\unpack-installer.ps1')
} catch {
    Fail "Unpacking failed.`n$($_.Exception.Message)`nYour installer is still at $dest. Fix the problem above and run .\build.ps1 again."
}
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    Fail "Unpacking failed with exit code $LASTEXITCODE. Your installer is still at $dest."
}

Write-Host ''
Write-Host '3/4  Compiling the 64-bit bridge'
try {
    Compile
} catch {
    Fail @"
The bridge did not compile.
$($_.Exception.Message)
Install Visual Studio 2022 Build Tools with the C++ compilers for x86 and x64, then run .\build.ps1 again.
"@
}

Write-Host ''
Write-Host '4/4  Building the setup program'
if ($OutputDir) { $OutputDir = Resolve-GivenPath $OutputDir }
try {
    if ($OutputDir) {
        & (Join-Path $root 'installer\build-installer.ps1') -SkipBuild -OutputDir $OutputDir
    } else {
        & (Join-Path $root 'installer\build-installer.ps1') -SkipBuild
    }
} catch {
    Fail @"
The setup program did not build.
$($_.Exception.Message)
Inno Setup 6 comes with this project in tools\InnoSetup. Restore that folder if ISCC.exe is missing, then run .\build.ps1 again.
"@
}

$setup = if ($OutputDir) { Join-Path $OutputDir $setupName } else { Join-Path $root $setupName }
if (-not (Test-Path -LiteralPath $setup)) {
    Fail "The build reported success but '$setup' is not there."
}
Write-Host ''
Write-Host 'Done.'
Write-Host "Install by double-clicking $setup"
