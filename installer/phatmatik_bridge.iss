; One-file installer for pHATmatik PRO on 64-bit hosts.
;
; Default build (bundled):  contains iZotope's original, unmodified installer (kept in dist\)
;                           and runs it for the user, then installs the 64-bit bridge. Written to
;                           the project root. For use by people who hold a licence for
;                           pHATmatik PRO - see README.md, "Sharing the installer".
; ISCC /DBridgeOnly:        contains only the bridge. Written to dist\. The original plug-in must
;                           already be installed, or iZotope's installer must sit next to this one.
;
; Command-line options in addition to Inno Setup's own:
;   /VST64DIR="folder"   64-bit VST2 folder to install the bridge into
;   /PLUGIN32="file"     use this existing 32-bit "iZotope pHATmatik PRO.dll"; iZotope's installer is not run
;   /CURRENTUSER         install for the current user only, without administrator rights
;                        (needs /PLUGIN32 or an existing installation of the original)

#define AppVer "1.0.1"
#define OrigName "iZotope_pHATmatik_PRO_v1_52.exe"
#define PluginFile "iZotope pHATmatik PRO.dll"
#define RegKey "SOFTWARE\pHATmatik PRO Bridge"

[Setup]
AppId={{8C5B7E0A-6C1D-4D0B-9E57-2F3B6C1D9A41}
AppName=pHATmatik PRO 64-bit Bridge
AppVersion={#AppVer}
AppVerName=pHATmatik PRO 64-bit Bridge {#AppVer}
AppPublisher=Unofficial bridge - not an iZotope product
DefaultDirName={autopf}\pHATmatik PRO Bridge
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
#ifdef BridgeOnly
OutputDir=..\dist
OutputBaseFilename=pHATmatik_PRO_x64_Bridge_Only_Setup
#else
OutputDir=..
OutputBaseFilename=pHATmatik_PRO_v1_52_x64_Setup
LicenseFile=..\extracted\app\License_en.rtf
#endif
InfoBeforeFile=before.txt
InfoAfterFile=after.txt
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
UninstallDisplayName=pHATmatik PRO 64-bit Bridge
VersionInfoVersion={#AppVer}
VersionInfoDescription=pHATmatik PRO 64-bit Bridge Setup (unofficial)
VersionInfoProductName=pHATmatik PRO 64-bit Bridge

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This installs iZotope pHATmatik PRO v1.52 so that it can be used in 64-bit hosts.%n%nThe original 32-bit plug-in is installed unchanged, together with a small 64-bit bridge that lets 64-bit hosts load it.%n%nThis package is not made or supported by iZotope.

[Files]
Source: "..\build\x64\{#PluginFile}"; DestDir: "{code:Vst64Dir}"; Flags: ignoreversion
Source: "..\build\x86\pHATmatikBridgeHost32.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "after.txt"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion
#ifndef BridgeOnly
Source: "..\dist\{#OrigName}"; Flags: dontcopy
#endif

[Registry]
; Read by the bridge DLL to find the helper and the original plug-in. HKLM for a normal
; installation, HKCU with /CURRENTUSER; the bridge looks in both.
Root: HKA; Subkey: "{#RegKey}"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#RegKey}"; ValueType: string; ValueName: "Host32"; ValueData: "{app}\pHATmatikBridgeHost32.exe"
Root: HKA; Subkey: "{#RegKey}"; ValueType: string; ValueName: "Plugin32"; ValueData: "{code:Plugin32Path}"
Root: HKA; Subkey: "{#RegKey}"; ValueType: string; ValueName: "Vst64Dir"; ValueData: "{code:Vst64Dir}"

[Code]
const
  BridgeMarker = 'pHATmatikBridge64-marker';

var
  VstPage: TInputDirWizardPage;
  Plugin32: String;

function InDir(Dir: String): String;
begin
  Result := '';
  if (Dir <> '') and FileExists(AddBackslash(Dir) + '{#PluginFile}') then
    Result := AddBackslash(Dir) + '{#PluginFile}';
end;

function IsBridgeFile(FileName: String): Boolean;
var
  Data: AnsiString;
begin
  Result := LoadStringFromFile(FileName, Data) and (Pos(BridgeMarker, Data) > 0);
end;

{ iZotope's installer lets the user pick the 32-bit VST folder and does not record the choice
  anywhere except its uninstall log, so look there too. }
function FromOriginalUninstallLog: String;
var
  AppPath: String;
  Data: AnsiString;
  P, S: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\iZotope pHATmatik PRO_is1',
                             'Inno Setup: App Path', AppPath) then
    Exit;
  if not LoadStringFromFile(AddBackslash(AppPath) + 'unins000.dat', Data) then
    Exit;
  P := Pos('\{#PluginFile}', Data);
  if P = 0 then
    Exit;
  S := P;
  while (S > 1) and (Data[S] <> ':') do
    S := S - 1;
  if S < 2 then
    Exit;
  Result := Copy(Data, S - 1, P - (S - 1) + Length('\{#PluginFile}'));
  if not FileExists(Result) or IsBridgeFile(Result) then
    Result := '';
end;

function FindPlugin32: String;
var
  S: String;
begin
  Result := ExpandConstant('{param:PLUGIN32|}');
  if Result <> '' then
  begin
    if not FileExists(Result) then
      Result := '';
    Exit;
  end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\VST', 'VSTPluginsPath', S) then
    Result := InDir(S);
  if Result = '' then Result := InDir(ExpandConstant('{commonpf32}\Steinberg\VstPlugins'));
  if Result = '' then Result := InDir(ExpandConstant('{commonpf32}\VstPlugins'));
  if Result = '' then Result := InDir(ExpandConstant('{commoncf32}\VST2'));
  if Result = '' then Result := FromOriginalUninstallLog;
  if (Result = '') and RegQueryStringValue(HKA, '{#RegKey}', 'Plugin32', S) and FileExists(S) then
    Result := S;
  if (Result <> '') and IsBridgeFile(Result) then
    Result := '';
end;

function DefaultVst64Dir: String;
var
  S: String;
begin
  Result := ExpandConstant('{param:VST64DIR|}');
  if Result <> '' then Exit;
  if RegQueryStringValue(HKA, '{#RegKey}', 'Vst64Dir', Result) and (Result <> '') then Exit;
  if not IsAdminInstallMode then
  begin
    Result := ExpandConstant('{userpf}\VstPlugins');
    Exit;
  end;
  if RegQueryStringValue(HKLM64, 'SOFTWARE\VST', 'VSTPluginsPath', S) and (S <> '') then
  begin
    Result := S;
    Exit;
  end;
  Result := ExpandConstant('{commonpf64}\Steinberg\VstPlugins');
  if DirExists(Result) then Exit;
  S := ExpandConstant('{commonpf64}\VstPlugins');
  if DirExists(S) then begin Result := S; Exit; end;
  S := ExpandConstant('{commoncf64}\VST2');
  if DirExists(S) then Result := S;
end;

function Vst64Dir(Param: String): String;
begin
  Result := RemoveBackslashUnlessRoot(VstPage.Values[0]);
end;

function Plugin32Path(Param: String): String;
begin
  Result := Plugin32;
end;

procedure InitializeWizard;
begin
  VstPage := CreateInputDirPage(wpInfoBefore,
    'Select 64-bit VST Plug-ins Folder',
    'Where does your 64-bit host look for VST2 plug-ins?',
    'The 64-bit bridge is installed into this folder as "{#PluginFile}". Choose the folder your ' +
    'host scans for 64-bit VST2 plug-ins, then click Next.' + #13#10#13#10 +
    'This must not be the folder that holds your 32-bit plug-ins.',
    False, '');
  VstPage.Add('');
  VstPage.Values[0] := DefaultVst64Dir;
end;

{ The bridge has the same file name as the original plug-in, so it must never be written over it. }
function Vst64DirProblem: String;
var
  Dir, Existing, S: String;
begin
  Result := '';
  Dir := Vst64Dir('');
  if Dir = '' then
  begin
    Result := 'Please choose a folder.';
    Exit;
  end;
  if CompareText(Dir, ExpandConstant('{commonpf32}\Steinberg\VstPlugins')) = 0 then
    Result := 'That is the default folder for 32-bit plug-ins.';
  if RegQueryStringValue(HKLM32, 'SOFTWARE\VST', 'VSTPluginsPath', S) and (CompareText(Dir, RemoveBackslashUnlessRoot(S)) = 0) then
    Result := 'That is the folder registered for 32-bit plug-ins.';
  if (Plugin32 <> '') and (CompareText(Dir, ExtractFileDir(Plugin32)) = 0) then
    Result := 'That folder holds the original 32-bit plug-in.';
  Existing := AddBackslash(Dir) + '{#PluginFile}';
  if (Result = '') and FileExists(Existing) and not IsBridgeFile(Existing) then
    Result := 'That folder already contains a file named "{#PluginFile}" that is not the bridge - probably the original 32-bit plug-in.';
  if Result <> '' then
    Result := Result + ' The bridge needs a separate folder that your 64-bit host scans.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Problem: String;
begin
  Result := True;
  if CurPageID = VstPage.ID then
  begin
    Problem := Vst64DirProblem;
    if Problem <> '' then
    begin
      { Not shown in a silent installation with /SUPPRESSMSGBOXES; Setup then just stops. }
      Log('64-bit VST folder rejected: ' + Problem);
      SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

function RunOriginalInstaller(Exe, Params: String): Boolean;
var
  Code: Integer;
begin
  { iZotope's installer needs administrator rights. }
  if IsAdmin then
    Result := Exec(Exe, Params, '', SW_SHOW, ewWaitUntilTerminated, Code)
  else
    Result := ShellExec('runas', Exe, Params, '', SW_SHOW, ewWaitUntilTerminated, Code);
  Log(Format('Original installer "%s" %s: started=%d exit code=%d', [Exe, Params, Integer(Result), Code]));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Exe, Picked: String;
begin
  Result := '';
  Plugin32 := FindPlugin32;
  if Plugin32 <> '' then
    Log('Original 32-bit plug-in already present: ' + Plugin32)
  else if ExpandConstant('{param:PLUGIN32|}') <> '' then
  begin
    Result := 'The file given with /PLUGIN32 does not exist.';
    Exit;
  end
  else
  begin
    Exe := '';
#ifndef BridgeOnly
    ExtractTemporaryFile('{#OrigName}');
    Exe := ExpandConstant('{tmp}\{#OrigName}');
#else
    if FileExists(ExpandConstant('{src}\{#OrigName}')) then
      Exe := ExpandConstant('{src}\{#OrigName}');
#endif
    if Exe <> '' then
    begin
      { Silently first: the licence was shown and accepted in this wizard. }
      RunOriginalInstaller(Exe, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LANG=en');
      Plugin32 := FindPlugin32;
      if (Plugin32 = '') and not WizardSilent then
      begin
        { That did not produce the plug-in; let iZotope's own wizard run instead. }
        RunOriginalInstaller(Exe, '/SP-');
        Plugin32 := FindPlugin32;
      end;
    end;
    if (Plugin32 = '') and not WizardSilent then
    begin
      Picked := '';
      if (SuppressibleMsgBox('Setup could not find the original 32-bit plug-in ("{#PluginFile}").' + #13#10#13#10 +
                 'If it is installed in a folder of your own choosing, click OK and select the file.',
                 mbConfirmation, MB_OKCANCEL, IDCANCEL) = IDOK) and
         GetOpenFileName('Select the original 32-bit {#PluginFile}', Picked, '', '{#PluginFile}|{#PluginFile}', 'dll') and
         not IsBridgeFile(Picked) then
        Plugin32 := Picked;
    end;
    if Plugin32 = '' then
    begin
#ifndef BridgeOnly
      Result := 'The original 32-bit pHATmatik PRO plug-in could not be installed or found, so the bridge would have nothing to load.';
#else
      Result := 'The original 32-bit pHATmatik PRO plug-in is not installed. Install it with iZotope''s installer ({#OrigName}), or put that installer next to this one, and run this setup again.';
#endif
      Exit;
    end;
  end;
  Result := Vst64DirProblem;
end;
