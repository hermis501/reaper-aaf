# AAF for REAPER

A REAPER extension that opens `.aaf` files as REAPER projects and saves REAPER projects as `.aaf`.

AAF is the format used to exchange edits between Avid Pro Tools, Avid Media Composer, Adobe Premiere Pro, DaVinci Resolve, Nuendo, Logic Pro and others.

Version {VERSION}, Windows (64-bit) and macOS (10.15 or later, Apple silicon and Intel), REAPER 6 or later (REAPER 7 recommended).

## Contents

- `reaper_aaf-x64.dll` (Windows) or `reaper_aaf.dylib` (macOS): the REAPER extension
- `command-line/aaf2rpp`, `command-line/rpp2aaf` and `command-line/aafdump` (`.exe` on Windows): the same conversions without REAPER, plus a tool that prints the structure of any AAF file (optional)
- `THIRD-PARTY-NOTICES.txt`

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

To uninstall, delete the extension file. The extension can be installed together with *DAWproject for REAPER* (https://github.com/hermis501/reaper-dawproject).

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
