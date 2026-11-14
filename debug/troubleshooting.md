# Troubleshooting

Problems that were actually met, with the cause that was found and the fix that worked. Add an entry whenever one is solved.

## Using the plug-in

### The plug-in does not load in the host

Read the last lines of `%TEMP%\PhatmatikBridge.log`. The bridge writes the reason there every time it refuses to load.

| Log says | Cause | Fix |
| --- | --- | --- |
| `the original 32-bit plug-in ... was not found` | The original DLL is not where the bridge was told to look | Run the installer again |
| `LoadLibrary failed with error 193` | The bridge was pointed at a 64-bit DLL instead of the original | Run the installer again |
| `the 32-bit helper is gone` | The helper process was ended while the plug-in was in use. The plug-in goes silent; the host keeps running | Remove the plug-in and add it again |

To also log every successful load, add a string value `Debug` = `1` under `HKEY_CURRENT_USER\SOFTWARE\Phatmatik PRO Bridge`. Remove it when done.

### The host seems frozen right after loading the plug-in

**Cause:** until it is authorized, the plug-in shows its "iZotope Authorization" window while it loads, and the host waits for it. That window can be behind the host's.

**Fix:** find it with Alt+Tab and answer it.

### The plug-in loads but is silent

Work through these in order. All of them are the plug-in's own behaviour, the same with and without the bridge.

1. **No loop is loaded, or nothing is playing it.** Load one from the built-in browser (below), then use the plug-in's play button or send MIDI notes from 48 upwards.
2. **Trial mode, and the authorization window did not appear when the plug-in loaded.** During the trial the plug-in shows its window at most about once a minute, and an instance loaded without it stays silent for as long as it lives. Remove the plug-in, wait a minute, add it again and click **Continue**. An authorized plug-in does not do this.
3. **The trial has ended.** The plug-in says "This Phatmatik PRO demo has expired. The plug-in will output silence until it is authorized." Authorize it with a serial number.

To see whether sound is leaving the host at all, run `tools\audio-meter.ps1`: it reports whether the Windows output is muted and which programs are sending sound to it.

### Loading a loop in the plug-in's browser

- The folder button at the top left opens the browser; the plug-in's window becomes wider.
- In the upper list, a **single** click opens a folder. Let the pointer rest on the row before clicking: the list acts on the row it last saw the pointer over.
- The list does not scroll with the mouse wheel. Drag its scrollbar.
- In the lower list, **double**-click a loop to load it. WAV, AIFF and pHATfiles load. The plug-in does not accept files dropped onto it.
- Press the slice button, the second in the toolbar, to cut the loop into slices.

### Notes in the piano roll play nothing

**Cause:** one of two things. The loop has not been sliced, so it is a single slice on MIDI note 48 (the plug-in's "Select All" button selects slices, it does not make them). Or the notes are on the wrong keys: slices start at MIDI note 48, which FL Studio calls C4.

**Fix:** press the slice button, then place notes from that key upwards. Each slice shows its note on its marker.

### MIDI dragged out of the plug-in does not arrive in FL Studio

**Cause:** FL Studio's playlist does not accept MIDI files, from the plug-in or from anywhere else; a `.mid` dragged from the Desktop is refused there too. It shows drop feedback and then does nothing.

**Fix:** drop on the channel rack. FL Studio opens its "Import MIDI data" dialog; accept it and the pattern receives one note per slice.

### The host freezes when MIDI is dragged out of the plug-in

**Cause:** bridge 1.0.0. The helper's drag was calling the host's drop target while the host was waiting on the helper, and neither could move.

**Fix:** install 1.0.1 or later by running the current installer over the existing installation. The installed version is shown in the properties of `iZotope Phatmatik PRO.dll` in the 64-bit VST folder, and as "Phatmatik PRO 64-bit Bridge" in Windows' installed apps.

### "Save as pHATfile" saved under a different name, or replaced my loop

**Cause:** the plug-in ignores the name typed in its dialog. It always writes `<loop name>.wav` into the folder the dialog opened in, and replaces a file of that name without asking. If that folder holds the original loop, the original is replaced by the pHATfile. Same without the bridge.

**Fix:** before pressing Save, browse to a folder that does not already contain the loop.

### Slices dragged out of the plug-in are missing

**Cause:** the plug-in hands the host a list of file names (`<sample export directory>\<loop>\<loop>_02.wav` and so on) and creates the `<loop>` folder, but writes no files into it. The same happens with the original plug-in loaded directly, with the export directory set or not, so it is the plug-in on this system and not the bridge.

**Fix:** none. Record the plug-in's output in the host instead.

### FL Studio lists the plug-in as "32+64" bits

**Cause:** FL Studio finds both the original 32-bit plug-in and the 64-bit bridge and combines them into one entry.

**Fix:** nothing to fix. FL Studio loads the 64-bit one. While the plug-in is loaded, a process named `PhatmatikBridgeHost32.exe` running under `FL64.exe` shows that the bridge is in use.

## Installing

### "That folder already contains a file named iZotope Phatmatik PRO.dll that is not the bridge"

**Cause:** the folder chosen for the bridge holds the original 32-bit DLL. The bridge has the same file name and would overwrite it, so the installer refuses.

**Fix:** choose the folder your 64-bit host scans, which must be a different folder.

### The plug-in installed, but 64-bit hosts still do not see it

**Cause:** the file that was run is iZotope's original installer, `iZotope_pHATmatik_PRO_v1_52.exe`. It installs only the 32-bit plug-in.

**Fix:** run `pHATmatik_PRO_v1_52_x64_Setup.exe` from the project root. `build.ps1` writes that file from your copy of iZotope's installer. If the original plug-in is already installed it is left as it is and only the bridge is added; there is no need to uninstall first.

### The installer build says iZotope's original installer is missing

**Cause:** `dist\iZotope_pHATmatik_PRO_v1_52.exe` is not there. The main installer contains that file, so it cannot be built without it. The file is left out of the repository on purpose.

**Fix:** run `.\build.ps1 -Installer "C:\path\to\iZotope_pHATmatik_PRO_v1_52.exe"` with your own copy. The script creates `dist\`, `extracted\`, and `build\` if the clone does not have them, then accepts only the v1.52 installer, SHA-256 `9205FA07ABB6B5388B2DBA0D1C0638AA8E3918D062F5CAD99428467663BB6A9E`. A different file is refused and the SHA-256 of both is printed. Copying the right file to `dist\iZotope_pHATmatik_PRO_v1_52.exe` and running `.\build.ps1` with no arguments is the other way.

## Developing

### "Decompile the .exe" yields an installer, not the product

**Cause:** the `.exe` is an Inno Setup installer. Its own code is the generic Inno stub.

**Fix:** run `scripts\unpack-installer.ps1`. It calls `innoextract -e` (1.9) on `dist\iZotope_pHATmatik_PRO_v1_52.exe` and writes `extracted\`. `innounp -x -m` also recovers the install script.

### innoextract prints "Unexpected data while converting from Windows-1252 to UTF-8"

**Cause:** the installer's Japanese strings are not Windows-1252. All files extract correctly. In PowerShell 5.1 the warning is shown as a red `NativeCommandError`; that is cosmetic.

### Build tools are not on PATH

**Cause:** `cl`, `rc` and `link` exist only inside a Visual Studio developer environment. `build.ps1` enters it for you; to do it by hand run `VsDevCmd.bat -arch=x86` or `-arch=amd64`.

### A test run hangs until its timeout

**Cause:** the plug-in is showing a dialog. The test host prints the dialog's title, text and buttons when it times out.

**Fix:** pass `--click "Continue,Demo,OK"` so the test host presses the plug-in's start-up prompts: "Continue" during the trial of an installed copy, "Demo" then "OK" for an expired or never-installed copy. An authorized plug-in shows none.

### An on-screen test fails once and passes the next time

**Cause:** someone touched the mouse or keyboard. `tests\gui.ps1` uses the real pointer and the real Alt and Ctrl keys; any other input lands in the middle of its clicks.

**Fix:** leave the machine alone while it runs. A test that keeps failing with nobody at the machine is a real failure.

### A browser click in a test opens the wrong folder

**Cause:** the plug-in's browser shows seven rows, sorted without regard to case, and a new folder on the drive shifts every row below it. A row beyond the seventh is only reachable after dragging the scrollbar.

**Fix:** do not hard-code row positions. `Open-Folder` in `tests\gui.ps1` works the clicks out from the real folder list and scrolls when it has to.

### A file-dialog step in a test never runs when the plug-in is loaded directly

**Cause:** with the plug-in in the host's own process, its dialog is modal on the host's UI thread, so the test's loop is stopped until the dialog closes.

**Fix:** the `filedlg` action answers the dialog from a separate thread. Schedule it just *before* the click that opens the dialog.

### A file saved by a test landed in the wrong place

**Cause:** the test pressed Save before the dialog had finished filling in its own default name, so the typed path was replaced.

**Fix:** the `filedlg` action re-reads the name box until it holds the intended path, cancels if it cannot, and never answers an overwrite question. See also the pHATfile entry above: that dialog ignores the typed name whatever the test does.

### A capture of the bridged editor is blank

**Cause:** `PrintWindow` does not reliably capture a child window that belongs to another process.

**Fix:** capture from the screen: run the test host with `--topmost --dpiaware` and a `shot` action.

### Posted mouse messages do nothing in the plug-in's editor

**Cause:** the plug-in's GUI library reads the real mouse button state, not the message.

**Fix:** use the test host's real-input actions (`click`, `dbl`, `drag`, `altdrag`, `ctrlclick`), which move the real pointer and press the real buttons.

### Backslashes vanish from scripts fed to Python through a Git Bash heredoc

**Cause:** in this environment a doubled backslash inside a heredoc reaches the program as a single one, so `\\n` becomes a newline.

**Fix:** write the script to a file first, or make the edit with an editor.

### A silent installer run shows a message box

**Cause:** `MsgBox` in an Inno Setup script is shown even with `/VERYSILENT /SUPPRESSMSGBOXES`.

**Fix:** use `SuppressibleMsgBox`. Fixed in `installer\phatmatik_bridge.iss`.
