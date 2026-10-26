# AAF for REAPER

A REAPER extension that opens `.aaf` files as REAPER projects and saves REAPER projects as `.aaf`.

AAF is the format used to exchange edits between Avid Pro Tools, Avid Media Composer, Adobe Premiere Pro, DaVinci Resolve, Nuendo, Logic Pro and others.

Windows (64-bit) and macOS (10.15 or later, Apple silicon and Intel), REAPER 6 or later (REAPER 7 recommended).

## Download

Get the latest zip for your system from [Releases](https://github.com/hermis501/reaper-aaf/releases):

- Windows: `AAF-for-REAPER-<version>-win64.zip`
- macOS: `AAF-for-REAPER-<version>-macos.zip` (one build for Apple silicon and Intel Macs)

Each contains:

- `reaper_aaf-x64.dll` (Windows) or `reaper_aaf.dylib` (macOS): the REAPER extension
- `command-line/aaf2rpp`, `command-line/rpp2aaf` and `command-line/aafdump` (`.exe` on Windows): the same conversions without REAPER, plus a tool that prints the structure of any AAF file (optional)
- `README.md`, `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt`

## Install

1. Close REAPER.
2. Copy the extension into REAPER's `UserPlugins` folder:
   - Windows: `reaper_aaf-x64.dll` into `%APPDATA%\REAPER\UserPlugins`
   - macOS: `reaper_aaf.dylib` into `~/Library/Application Support/REAPER/UserPlugins` (in Finder: *Go > Go to Folder...*)

   In REAPER, *Options > Show REAPER resource path in explorer/finder* opens the resource folder that contains `UserPlugins`. For a portable install, use the `UserPlugins` folder next to the REAPER program.
3. Start REAPER.

**macOS:** the extension is not notarized by Apple. If REAPER does not load it (the File menu entries are missing, or macOS says the file can't be verified), remove the download quarantine in Terminal and restart REAPER:

```
xattr -dr com.apple.quarantine ~/Library/Application\ Support/REAPER/UserPlugins/reaper_aaf.dylib
```

To uninstall, delete the extension file. The extension can be installed together with [DAWproject for REAPER](https://github.com/hermis501/reaper-dawproject).

## Opening an AAF file

Use *File > Open project* and choose *AAF files* as the file type, or drag an `.aaf` file onto REAPER. The edit opens as an unsaved REAPER project. Save it as `.rpp` to keep it. Anything that could not be converted is listed in the REAPER console (ReaScript console window) after loading.

Media:

- Audio files referenced by the AAF are looked for:
  1. where the AAF says they are
  2. next to the AAF, in the same folder layout
  3. by a search in the folders below the AAF
- Audio embedded in the AAF is extracted to a folder named `<aaf name>-media` next to the AAF. If that folder can't be written, it goes to the temp folder.
- Multichannel clips that are made of several mono files are combined into one multichannel WAV file in that folder.

What is converted:

- **Tracks:** one REAPER track per audio track, with name, volume, pan, mute, solo, and volume and pan automation. Media Composer's master fader becomes the master track.
- **Clips:**
  - position, source offset, clip gain and clip gain automation
  - fades and cross-fades (linear or equal power), muted clips
  - stereo and surround clips, single channels of multichannel files
  - speed changes
- **Subclips and nested sequences:** resolved down to the audio files.
- **Pro Tools:** rendered fade clips and "sample accurate edit" pieces become real fades and continuous clips again.
- **Markers:** markers and ranged markers (as regions), with names and colors.
- **Start time:** the sequence's start timecode becomes the project start offset.

Not converted: video tracks, effects other than gain, pan and speed, and Avid EQ/plug-in data.

## File menu

The extension adds two entries to REAPER's *File* menu, next to *Export project MIDI...*:

- **Export project to AAF (media referenced)...**
- **Export project to AAF (media embedded)...**

Opening AAF files is part of *File > Open project*.

The same commands are in the Actions list, for shortcuts, toolbar buttons and scripts:

| Action | Command ID |
|---|---|
| AAF: Export project to AAF (media referenced)... | `_DAWPROJECT_EXPORT_AAF` |
| AAF: Export project to AAF (media embedded)... | `_DAWPROJECT_EXPORT_AAF_EMBED` |

Scripts can call them with `reaper.Main_OnCommand(reaper.NamedCommandLookup("_DAWPROJECT_EXPORT_AAF"), 0)`.

If you have customized your File menu (*Options > Customize menus/toolbars*), REAPER keeps your layout and the entries are not added automatically. Add the actions to the menu there, or reset the File menu to its default.

## Saving a REAPER project as AAF

There are two entries in the *File* menu:

- **Export project to AAF (media referenced)...**: the AAF points to your audio files where they are. The result is small, but the audio files have to travel with it.
- **Export project to AAF (media embedded)...**: WAV and AIFF files are copied into the AAF. The result is one self-contained file.

A save dialog asks for the file name. When the export is done, the REAPER console shows the status: the saved file, the number of tracks, clips and media files, and anything that could not be exported or was approximated. Your project and its file name are not changed.

The file follows the AAF Edit Protocol, the layout Pro Tools and Media Composer exchange: a timecode track, one audio track per REAPER track, and a master clip for every audio file.

What is exported:

- **Tracks:** name, volume and pan (constant or automated), mute, solo. Folders are flattened.
- **Items** (active take):
  - position, length and source offset
  - item and take volume, and the take volume envelope, as clip gain
  - fades (linear, or equal power for curved REAPER fades)
  - cross-fades between overlapping items that fade into each other
  - muted items
  - looped items, as one clip per repetition
  - section sources
  - channel modes: left, right, single channel, reversed stereo
- **Extra tracks:** AAF tracks can't hold overlapping clips or mix mono and stereo clips. Items that overlap without a cross-fade, or that have a different channel count, go to extra tracks named "Track (2)", "Track (3)" and so on.
- **Markers and regions:** regions become ranged markers.
- **Timing:** the project start offset becomes the start timecode, along with the project frame rate and sample rate.

Things to know:

- **Play rate** is written as a speed change effect. Some applications ignore that effect and play the clip at normal speed. If that matters, glue or render the item first.
- **File formats:** use WAV or AIFF audio for exchange. Other formats (FLAC, MP3, OGG…) are referenced but described only roughly, and most other applications will not open them.
- **Sample rate:** clips are placed at the project sample rate (Project settings).

Not exported: MIDI, FX and sends (AAF carries no plug-ins or routing), the master track's volume, pan and FX, take pan and pitch envelopes, other takes, reversed playback, mute and width envelopes. Curved envelope shapes become linear.

## Command-line tools (optional)

Open a command prompt (Windows) or Terminal (macOS) in the `command-line` folder. On macOS, start the tools with `./` (for example `./aaf2rpp`); if macOS blocks them, run `xattr -dr com.apple.quarantine .` in that folder first.

```
aaf2rpp edit.aaf [edit.rpp] [--media <folder>] [--pt-rendered-fades]
rpp2aaf song.rpp [song.aaf] [--embed]
aafdump edit.aaf
```

- `aaf2rpp --media` sets the folder for extracted audio.
- `--pt-rendered-fades` keeps Pro Tools' rendered fade clips as they are.
- `rpp2aaf --embed` copies WAV/AIFF files into the AAF.
- `aafdump` prints the object tree of an AAF file, which is useful when an AAF from another application doesn't convert as expected.

## Notes

The importer was checked clip by clip against the test files of LibAAF (exports from Media Composer, Pro Tools, Premiere Pro and DaVinci Resolve). Exported files were checked with LibAAF and pyaaf2. If an application refuses an exported file or shows something wrong, please report it with the application's name and version, and with its error message if there is one.

The macOS version is built and tested automatically (conversion tests and command-line tools), but it has not yet been tried inside REAPER on a Mac. Please report any problem.

## Building from source

Requires CMake 3.20+ and a C++17 compiler: Visual Studio 2019 or later on Windows, or Xcode or its command line tools on macOS (the macOS build is universal: arm64 and x86_64, macOS 10.15+). Python 3 for the tests and packaging.

```
cmake -S . -B build -A x64                 # on macOS: cmake -S . -B build
cmake --build build --config Release
cmake --install build --config Release     # copies the extension into REAPER's UserPlugins folder (close REAPER first)
python tools/make_package.py               # dist/ folder and zip, as published in Releases
```

The binaries are in `build/Release`: the extension (`reaper_aaf-x64.dll` or `reaper_aaf.dylib`) and the command-line tools (`aaf2rpp`, `rpp2aaf`, `aafdump`, with `.exe` on Windows).

## Tests

```
python tests/test_aaf.py          # export round trip, plus the LibAAF import comparison if configured
```

`test_aaf.py` takes an optional path to the binaries (default `build/Release`). With `AAFTOOL` (the `aaftool` of [LibAAF](https://github.com/agfline/LibAAF)) it checks the exported files with LibAAF, and with `LIBAAF_TEST_DIR` (LibAAF's `test/aaf` folder) it imports all of LibAAF's test files and compares them clip by clip with aaftool. With [pyaaf2](https://github.com/markreidvfx/pyaaf2) installed, the exported files are also opened with pyaaf2. The workflow does all three.

## Releases

The GitHub Actions workflow (`.github/workflows/build.yml`) builds and tests every push and pull request on Windows and macOS and uploads both packages as build artifacts. Pushing a tag such as `v0.1.1` also creates a GitHub release with both zips attached. Set the same version in `CMakeLists.txt` first:

```
git tag v0.1.1
git push origin v0.1.1
```

## Source layout

- `src/` – the extension and conversion code
  - `aaf_import.*` – AAF → intermediate XML; `converter.*` – that XML → RPP
  - `aaf_export.*` – RPP → AAF
  - `aaf_file.*`, `cfb.*`, `aaf_model_data.inc` – AAF objects on a compound file (structured storage) implementation
  - `reaper_main.cpp`, `reaper_ext.*` – REAPER registration, File menu, console
- `cli/` – the command-line tools
- `tests/` – automated tests
- `packaging/README.md` – the README that goes into the release zip
- `third_party/` – bundled libraries with their licenses (`WDL/swell`: Cockos' SWELL headers, used by the macOS build)
- `tools/` – packaging, model table generator

## License

MIT, see [LICENSE](LICENSE). Bundled third-party code keeps its own license (see `third_party/` and `THIRD-PARTY-NOTICES.txt` in the release zip).
