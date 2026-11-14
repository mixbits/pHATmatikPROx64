# Changelog

Completed and validated work, newest first. Everything listed was run and observed on 64-bit Windows 11; each entry says where.

## 2026-11-13 — one deployment command

`build.ps1` is the deployment command. On Windows 11, `.\build.ps1 -Installer` with a missing path created no files and stopped, naming `.\build.ps1` as the command to run again. `.\build.ps1 -CompileOnly -Installer` stopped before compiling and told the two switches not to be combined. `.\build.ps1 -CompileOnly` compiled into `build\` and printed `Build OK`; the setup program in the project folder kept SHA-256 `D08C479A471139F76ADD9B2C3C8569DFAF4AE6D6E6C261659806E5BB95F21C0C`. `.\build.ps1 -OutputDir build\test\isscheck` used the installer already in `dist\`, unpacked, compiled, and wrote both setup programs into that folder, then that folder was removed. `scripts\make.ps1` calls `build.ps1`.

## 2026-11-13 — folders

`scripts\make.ps1` creates `dist\`, `extracted\`, `build\`, `build\x64\`, `build\x86\`, `build\obj\`, and `build\test\` when they are missing. Observed on Windows 11 by renaming `build` aside and running `.\scripts\make.ps1 -Installer` with a path that does not exist. It printed `Created build\, build\x64\, build\x86\, build\obj\, build\test\`, then stopped because that path had no file, and said to copy the installer to `dist\iZotope_pHATmatik_PRO_v1_52.exe`. `build` was put back afterwards. `images\screenshot.png` is listed in `.gitignore`.

## 2026-11-13 — setup script

`scripts\make.ps1` on Windows 11, with innoextract 1.9 from `tools\innoextract`.

- **Refused, and left `dist\iZotope_pHATmatik_PRO_v1_52.exe` unchanged:** a missing path, `README.md`, `dist\pHATmatik_PRO_x64_Bridge_Only_Setup.exe`, and the `dist` folder. The wrong program printed the expected SHA-256 `9205FA07ABB6B5388B2DBA0D1C0638AA8E3918D062F5CAD99428467663BB6A9E` and that file's own SHA-256.
- **`.\scripts\make.ps1 -Installer`** pointed at `dist\iZotope_pHATmatik_PRO_v1_52.exe`. The hash matched, the file was not copied over itself, `extracted\` was unpacked, the bridge compiled, and both setup programs were written.
- **`.\scripts\make.ps1`** with no arguments, using the installer already in `dist\`. It printed `Using D:\Projects\VST\dist\iZotope_pHATmatik_PRO_v1_52.exe` and finished the same way. The setup programs from this run are the ones now in the project folder:
  - `pHATmatik_PRO_v1_52_x64_Setup.exe` — SHA-256 `D08C479A471139F76ADD9B2C3C8569DFAF4AE6D6E6C261659806E5BB95F21C0C`
  - `dist\pHATmatik_PRO_x64_Bridge_Only_Setup.exe` — SHA-256 `330D8DA8B5E95E52623E2358F46DA96AA7690D3B2808ED14D3B8432EF9767885`
- These two files were not installed. They replace the setup programs recorded under 1.0.1 below. A second compile does not reproduce those earlier hashes. innoextract printed its Windows-1252 warning both times and the unpack still finished.

## 1.0.1 — 2026-11-13

The current version. It is installed on the development machine and confirmed in FL Studio 2026.

### Fixed

- **Dragging MIDI out of the plug-in froze the host.** With 1.0.0, Alt + drag from the MIDI export button to the host stopped both processes: the helper's drag was calling the host's drop target while the host's UI thread was waiting on the helper. Both sides now wait with `waitPumping` (`src\common\ipc.h`), which keeps answering incoming drag-and-drop calls and sent window messages. The same drag now delivers the MIDI file, identical to the one the export dialog writes.

### Validated in FL Studio 2026 (64-bit)

Driven by the owner; checked against the running processes, the bridge log, the Windows audio meter and the saved project file.

- **Scan.** The Plugin Manager lists "iZotope Phatmatik PRO", status ok, as "32+64" bits.
- **Load.** FL Studio loads it through this project's bridge: `PhatmatikBridgeHost32.exe` runs under `FL64.exe`, and the bridge log shows the load.
- **Window.** The plug-in's window opens inside FL Studio and grows to 850 pixels when its built-in browser is opened.
- **Loop.** A WAV loop loads from the browser and the slice button cuts it into slices labelled from C (48) to F (65).
- **Sound.** Notes from the piano roll and the keys play the slices; the play-preview button plays the loop. FL Studio's audio session measured a peak of 0.457 on the Windows output.
- **MIDI out.** The export button saves a MIDI file through its dialog. Alt + drag from the plug-in onto the channel rack opens FL Studio's "Import MIDI data" dialog and fills the pattern with one note per slice.
- **Two instances** in one project, each with its own helper.
- **Save and reopen.** The saved project carries the plug-in's ID `ppro` and its state for both instances, and reopens with the sliced loop playing.
- **Clean session.** Three loads by FL Studio, every helper closed through the normal path, no errors in the log.

### Validated against the original plug-in

Each test runs twice in the project's test hosts, with the original 32-bit plug-in loaded directly and through the bridge. Results must be identical, and are.

- **`tests\features.ps1`, 11 of 11.** From a saved state with a sliced loop: 2000 blocks of sound; all 48 parameters at four values each (values, display text and audio); the 16 programs; 48 kHz at 97 bpm, 96 kHz at 140 bpm and 22.05 kHz at 174 bpm; 8192-sample blocks; parameter changes during playback; six instances on six threads; 25 editor open and close cycles; and a 200,000-block soak, about 39 minutes of audio.
- **`tests\gui.ps1`, 9 of 9**, with real mouse and keyboard input: MIDI export through its dialog; MIDI dragged to the host; slices dragged to the host; the play-preview button; a knob drag reaching the host on its main thread; the browser button resizing the window to 850×450; an AIFF file loaded, sliced and played; Save as pHATfile; and that pHATfile loaded back with its slices. Files written are byte-identical.
- **`tests\real-audio.ps1`.** The plug-in's saved state restored into a fresh instance and played: the two rendered WAV files are byte-identical.
- **`tests\compare.ps1`, 7 of 7**, with the validation plug-in: audio, MIDI out including system exclusive, parameters, presets and host callbacks, for block sizes from 1 to 40,000 samples.
- **`tests\compare.ps1 -Phatmatik -Installed`, 3 of 3**: the installed bridge against the installed original.
- **`tests\robustness.ps1`, 7 of 7**: helper ended mid-session (host keeps running, output silent, clean close); host ended without closing the plug-in (no helper left); eight instances at once; a missing plug-in and a wrong-architecture DLL (refused, reason logged).

### Installer

- **Install over 1.0.0**, run by the owner with administrator rights: the installer found the original plug-in in place, left it alone, and replaced the bridge. The installed bridge and helper are byte-identical to the tested build.
- **Built from the tested binaries.** Unpacked and compared: the packaged bridge, helper and iZotope installer are byte-identical to their sources. The build is reproducible; the same inputs give the same files.
  - `Phatmatik_PRO_v1_52_x64_Setup.exe` — SHA-256 `5902056C35D05268FCE484F81621563C00B42005F28B767245D90CD87D922503`
  - `dist\Phatmatik_PRO_x64_Bridge_Only_Setup.exe` — SHA-256 `DF259003F55BD0C60200BE22819B2896344D326478ACE04A57AB9287D4E4DDF0`

### Established about the plug-in itself

Seen the same with the original loaded directly, so none of it comes from the bridge.

- Installed by iZotope's installer it runs a 10-day full trial, showing its authorization window at most about once a minute; an instance loaded without that window stays silent. Authorized, it loads without any window and always plays.
- Loops load only from its built-in browser: a single click opens the folder under the pointer, a double click loads a file, the list scrolls by dragging its scrollbar. It loads WAV, AIFF and pHATfiles and refuses files dropped onto it.
- Slices answer to MIDI notes from 48 upwards. A saved state names the loop's file.
- A MIDI drag carries one file as `CF_HDROP`, copy only.
- "Save as pHATfile" writes `<loop name>.wav` (24-bit, with a `pHAT` chunk) into the folder its dialog opened in, whatever name is typed.
- Slices dragged out arrive as file names; the plug-in writes no audio files for them.
- FL Studio's playlist refuses MIDI files from any source; its channel rack accepts them.

### Tools and tests added

- `scripts\unpack-installer.ps1`. With innoextract 1.9 it unpacked `dist\iZotope_pHATmatik_PRO_v1_52.exe` into `build\test\unpack-check` (21 files). The plug-in DLL, `app\License_en.rtf`, `sys\REX Shared Library.dll` and `tmp\DemoInstall.dll` matched the copies already in `extracted\`. innoextract printed its Windows-1252 warning; the script still finished.
- `tests\features.ps1`, `tests\gui.ps1`, `tests\make_aiff.py`.
- Test host: scripted real-input actions (`click`, `dbl`, `drag`, `altdrag`, `ctrlclick`), file-dialog answering, a drop target on the host window, parameter sweep, program walk, editor cycles, sample rate and tempo options.
- `tools\audio-meter.ps1`: whether the Windows output is muted and which programs are sending sound to it.

## 1.0.0 — 2026-11-13

The first complete version: the bridge, the helper, the installers and the test tools.

### Built

- `src\bridge64` → `iZotope Phatmatik PRO.dll`, the 64-bit VST2 plug-in that hosts load.
- `src\host32` → `PhatmatikBridgeHost32.exe`, the 32-bit helper that runs the original plug-in.
- `src\testhost`, `src\testplug` → 32-bit and 64-bit test hosts and a validation plug-in.
- `installer\` → the main installer, which contains iZotope's original installer, and a bridge-only installer.
- `build\x64\Phatmatik PRO 64-bit.exe` → the test host built as a windowed program: it opens the installed plug-in with live sound, under the project's own icon.

### Validated

- **Fresh install by the owner with administrator rights.** iZotope's installer ran silently inside ours and exited with code 0. Afterwards the original plug-in, the REX library, the bridge, the helper and the registry values were all in place, and both products appeared in Windows' installed apps.
- **Per-user install and uninstall** (`/CURRENTUSER`): files and registry values written, the installed bridge loaded and matched the original, uninstall removed everything.
- **Overwrite protection.** The installer refuses a folder that already holds the original DLL, and leaves that file untouched.
- **The original plug-in on Windows 11.** Loaded directly in a 32-bit host: 48 parameters, 16 programs, 8 outputs, editor 640×450.
- **The bridge against the original**: parameters, presets, host callbacks and output identical; the editor pixel-identical inside a 64-bit host window; a real mouse drag on a knob reaching the host on its main thread.
- **Real Phatmatik sound.** A loop loaded through the plug-in's own browser, sliced and played, directly and through the installed bridge: identical state and identical audio.

### Fixed during development

- MIDI the plug-in sent from inside an events call was dropped; it is now delivered after every audio request.
- The installer's "wrong folder" message appeared even in a silent run; it now respects `/SUPPRESSMSGBOXES`.

## Project set-up — 2026-11-13

- **Investigation.** `iZotope_Phatmatik_PRO_v1_52.exe` is a 32-bit Inno Setup installer with a valid iZotope signature (SHA-256 `9205FA07ABB6B5388B2DBA0D1C0638AA8E3918D062F5CAD99428467663BB6A9E`). Its payload, unpacked into `extracted\`, is a 32-bit VST 2.4 plug-in DLL built in 2007, Propellerhead's REX library, and documentation.
- **Approach chosen by the owner:** bridge the unmodified 32-bit plug-in instead of reconstructing its source.
- **Layout chosen by the owner.** The main installer sits in the project root; iZotope's original installer sits in `dist\` beside the bridge-only installer, and the installer build reads it from there.
- **Project icon.** The plug-in has no icon of its own. `tools\draw-app-icon.ps1` draws one for the project, with no iZotope artwork in it; it is used in the README and on the windowed program.
- **README** opens with installation and use for end users; project details follow.
- **Repository policy from the owner:** a private repository that contains everything; `.gitignore` lists only `build\`.
