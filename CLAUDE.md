# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`reaper_aaf`: a REAPER extension that registers `.aaf` import (File > Open project) and *File > Export project to AAF (media referenced / embedded)...* (also the actions `_DAWPROJECT_EXPORT_AAF`, `_DAWPROJECT_EXPORT_AAF_EMBED`; the IDs keep their original prefix so existing shortcuts work), plus the CLIs `aaf2rpp`, `rpp2aaf` and `aafdump`. The DAWproject extension lives in its own repository (reaper-dawproject) and shares no code with this one; `converter.*` here is a trimmed copy of its DAWproject → RPP converter (no plug-ins, built-in devices, ZIP container, VCAs or post-load steps), and `util`, `rpp_reader`, `rpp_sink`, `tempo_map`, `media_info`, `reaper_ext` started as the same code, so a fix there may be worth porting by hand. User-facing docs: README.md (repository) and packaging/README.md (release zip).


## Commands

```
cmake -S . -B build -A x64                  # configure (Visual Studio generator)
cmake --build build --config Release        # DLL + aaf2rpp.exe + rpp2aaf.exe + aafdump.exe in build/Release
cmake --install build --config Release      # copies the DLL to %APPDATA%\REAPER\UserPlugins (REAPER must be closed)
python tools/make_package.py                # dist/AAF-for-REAPER (+ zip); user README in packaging/
python tests/test_aaf.py                    # AAF export round trip (+ LibAAF import comparison if configured)
build/Release/aaf2rpp.exe in.aaf out.rpp --media <dir> [--xml hub.xml]
build/Release/rpp2aaf.exe in.rpp out.aaf [--embed]
build/Release/aafdump.exe in.aaf            # object tree of any AAF
```

`test_aaf.py` uses `AAFTOOL` (LibAAF's aaftool, built from https://github.com/agfline/LibAAF) to check exported files and, with `LIBAAF_TEST_DIR` (LibAAF's `test/aaf`), compares every import clip by clip with aaftool (`--pt-true-fades --pt-remove-sae`, which aaf2rpp does by default). Run it with a Python that has pyaaf2 installed to also open the exported files with pyaaf2. All 65 LibAAF files pass; the known LibAAF differences are listed in the test.


## Architecture

macOS: the same sources build a universal (arm64 + x86_64, macOS 10.15+) `.dylib` and CLIs; CI (`.github/workflows/build.yml`) builds and tests on `windows-latest` and `macos-latest` and a `v*` tag releases both zips (`-win64`, `-macos`). On macOS `reaper_plugin.h` includes Cockos' SWELL headers (`third_party/WDL/swell`, unmodified, zlib license) and `swell-modstub.mm` (with `SWELL_PROVIDED_BY_APP`) looks up the Win32-like functions in REAPER at load time; `reaper_ext.cpp`'s menu code uses only calls SWELL has (`GetMenuItemInfo`, `InsertMenuItem`, ...). Apple's libc++ lacks floating-point `to_chars`/`from_chars` before macOS 13.3, so `util.cpp` formats/parses doubles with `snprintf_l`/`strtod_l` in the C locale there. `tools/make_package.py` ad-hoc signs the macOS binaries. Not yet tried inside REAPER on a Mac.

Sources are in `src/`, CLIs in `cli/`. The workflow `.github/workflows/build.yml` builds LibAAF's aaftool, installs pyaaf2, tests, packages, and creates a release for `v*` tags.

- Export commands are actions (a REAPER menu item runs an action) registered by `RegisterExportCommands` (`reaper_ext.cpp`, one copy per plug-in), which also adds them to the default File menu from `hookcustommenu` ("Main file", flag 0), after *Export project MIDI...* (40849), else after *Render...* (40015), else at the end; both DLLs' entries stay together. A user-customized File menu does not get them automatically. Command IDs are unchanged from before the move to the File menu (`_DAWPROJECT_EXPORT`, `_DAWPROJECT_EXPORT_AAF`, `_DAWPROJECT_EXPORT_AAF_EMBED`).
- Timeline walking uses a `View`: a mapping from a timeline's local time to absolute time (`offset`, in beats or seconds) plus a visible window in absolute **seconds** (`ws`/`we`) with fades and mute. `Walk` dispatches by element name; `ProcessClip` narrows the window, handles `playStart`/`playStop`, loops (one sub-view per loop pass) and `reference` (alias clips via the id index). Leaves (`AudioLeaf`, `WarpsLeaf`, `NotesLeaf`, `ExpressionLeaf`, `PointsLeaf`) emit REAPER items/envelopes clipped to the window. Each clip collects its notes/MIDI expressions into one `MidiCollector` → one MIDI item.
- All positions written to RPP are seconds computed through `TempoMap` (piecewise constant/linear-in-beats ramps); MIDI events are ticks (960/QN) relative to the item start in beats.
- `rpp_sink.h`: converter output goes through `RppSink` (REAPER's `ProjectStateContext` in the DLL, an indented file in the CLI). Lines starting with `<` open a block, `>` closes it.
- AAF layer: `src/cfb.cpp` is a compound file (structured storage) reader/writer (v4, 4K sectors on write). `src/aaf_file.cpp` reads/writes AAF objects; the baseline classes/types come from `aaf_model_data.inc` (generated from pyaaf2's MIT model tables by `tools/gen_aaf_model.py`). The writer builds the meta dictionary from the classes/types actually used; `DefineProperty` adds Avid extension properties (dynamic pids counting down from 0xFFFF). The reader finds extension properties by name (`PidByName`).
- AAF import (`src/aaf_import.cpp`): the top-level composition (UsageCode TopLevel, else a composition no other mob references) is walked into a clip model, then written as DAWproject-style XML with x-extensions (`Project xTimeOffset/xMetaData`, Clip `xGain/xFadeInShape/xFadeOutShape/xGainEnvelope`, `Marker xDuration`, `Audio xChannel`) that `src/converter.cpp` (AAF's own trimmed copy of the DAWproject converter) turns into RPP. SourceClip `StartTime` is in the referencing slot's edit rate; Transitions overlap their neighbours (position -= length); an OperationGroup that is the slot's segment (possibly inside other OperationGroups) applies to the track; control point times are 0..1 of the operation; a Selector whose Selected is a Filler is a muted clip (alternate[0]). Pro Tools' rendered fade clips and "Sample accurate edit" clips are replaced/merged as in LibAAF's ProTools.c.
- AAF export (`src/aaf_export.cpp`) reads the RPP directly (not through DAWproject XML). Audio slots run at the project sample rate; clip structure from the inside out: SourceClip (or Audio Channel Combiner of one SourceClip per channel) → Motion Control/SpeedRatio for play rates → Mono Audio Gain (always present on muted clips: LibAAF only ends a disabled clip's mute state there) → Selector with `_DISABLE_CLIP_FLAG` for muted items. Fades: `Filler, Transition(fade in), clip` and `clip, Transition(fade out), Filler`; cross-fades: `clip, Transition(overlap), clip` (LibAAF tells fade in/out/cross-fade apart by Filler neighbours). Lanes hold one channel count; overlapping items without a cross-fade and other channel counts go to extra slots. MasterMob slot i → file SourceMob slot i, PhysicalTrackNumber = channel + 1 (LibAAF takes the channel from the MasterMob slot). WAVE/AIFC summaries are rebuilt headers (format chunk copied, then the data chunk header); embedding stores the whole file as the EssenceData stream; ContentStorage always has an EssenceData set (LibAAF requires it).
- Paths are UTF-8 everywhere; `util.cpp` converts to wide APIs on Windows. Numbers are formatted/parsed with `to_chars`/`from_chars` (locale-independent).


## REAPER conventions

Confirmed from real project files:
- Mute envelope: 1 = unmuted, 0 = muted. Default mute envelopes are `PT 0 1 1`.
- Volume envelope points are amplitudes even with `VOLTYPE 1`; the values seen top out at exactly 2.0 (+6 dB).

AAF conventions confirmed from real files (LibAAF test set):
- Track mute/solo: TimelineMobSlot `TimelineMobAttributeList` TaggedValues `AudioMixerCompMute`/`AudioMixerCompSolo` (Int32).
- Media Composer fade shape: OperationGroup `ComponentAttributeList` `_ATN_AUDIO_DISSOLVE_CURVETYPE` (1 = equal power); others use the Level parameter's interpolation (Log/Power → equal power).
- Pro Tools: Header without OperationalPattern; ContainerFormat AAF even for external files; WAVE summary = the file's header up to the data chunk header.
- DaVinci Resolve writes Channel Combiner inputs shorter than the clip: the OperationGroup's Length is the clip length. Marker names are in `CommentMarkerUser`.

Not yet confirmed (no Pro Tools / Media Composer here): whether Pro Tools and Media Composer accept the exported files; how they treat the Motion Control speed effect and PCM descriptors for FLAC/MP3.


## Reference material

- AAF: LibAAF (GPL-2.0-or-later; reference and test tool only, don't copy code) — `src/AAFIface/AAFIParser.c`, `ProTools.c`, and its `test/aaf` files with `test/expected` output; pyaaf2 (MIT) for the object model and a second reader in tests; the AAF Object Specification / Edit Protocol for semantics
- REAPER SDK `reaper_plugin.h` (`project_import_register_t`) is vendored in `third_party/reaper-sdk`
