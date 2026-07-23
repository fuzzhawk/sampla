# Granular Sampler

A 3-layer granular sampler VST2 instrument for Ableton Live 9 on Windows 10,
with a native Win32 GUI. Built entirely on GitHub Actions — no local toolchain
required.

## Status: Milestone 2 — GUI + sampler engine

Loads as an Instrument with a **native editor** and three independent sample
layers, stacked and played monophonically from MIDI. Each layer:

- **Loads a WAV** — drag a `.wav` onto a lane, or double-click its waveform to
  browse. The lane shows the **waveform**, the **loop region**, draggable
  **loop-start / loop-end** handles, and a live **playhead**.
- **Loop modes** (click the Mode button to cycle):
  - **OneShot** — play once, no loop.
  - **Forward** — loop the tail region with a crossfaded seam (Overlap).
  - **Granular** — a grain cloud over the loop region, windowed per grain,
    with grain **size** and **density** controls.
- **Play from start / from loop** — "From start" plays the head of the sample
  once, then rolls into the loop / grain cloud at loop-start; "From loop"
  starts right in the loop.
- **Amplitude ADSR** (Atk / Dec / Sus / Rel) — the envelope for the layer,
  also the granular envelope in Granular mode.
- **Volume** and **fine-tune** (±100 cents) knobs. Note pitch drives playback
  rate; C4 (note 60) plays at native pitch.

A global **Master** volume sits at the top. Everything is exposed as an
automatable VST parameter (40 in total). Sample **file paths** are saved with
the preset/Live set and reloaded on recall.

> Formats: 16/24/32-bit PCM and 32/64-bit float WAV, mono or stereo. Voicing is
> monophonic (one note drives all three layers). AIFF/compressed formats and
> polyphony are future milestones.

## Layout

    src/vst2.h                    minimal VST2 ABI header (only dependency)
    src/wav.h                     clean-room WAV loader
    src/engine.h                  sampler + granular DSP (platform-neutral)
    src/editor.h                  native Win32/GDI editor
    src/plugin.cpp                VST glue: params, MIDI, chunk state, dispatch
    test/host.c                   console host for the CI smoke test
    test/engine_test.cpp          native DSP unit test (real audio path)
    Makefile                      builds x64 + x86 DLLs, hosts, engine test
    .github/workflows/build.yml   compile, engine test, Wine smoke test, artifacts

## Browser-only workflow

1. Commit any change through the GitHub web editor (or press `.` for
   github.dev).
2. The `build` workflow compiles both DLLs, runs the native engine unit test,
   and runs the 64-bit DLL under Wine in a minimal host — verifying it loads
   and answers the VST2 dispatcher.
3. Green check → download `GranularSampler-dlls` from the run's page under
   the Actions tab.
4. Copy the DLL matching your Live install (x64 for 64-bit Live) into the
   folder set in Live: Preferences → File/Folder → Plug-In Sources.
5. Live scans it on rescan/restart; it appears under Plug-ins as an
   Instrument. Drop it on a MIDI track, open its editor, load samples, and play.

For keeper builds: Releases → Draft a new release → publish. The workflow
attaches both DLLs to the release automatically.

## Using the editor

- **Load:** drag a `.wav` onto one of the three lanes, or double-click a lane's
  waveform to open a file browser.
- **Loop points:** drag the two yellow handles in the waveform.
- **Mode / Play:** click the **Mode** button to cycle OneShot → Forward →
  Granular; click the **From start / From loop** button to toggle.
- **Knobs:** drag any knob **vertically** to change its value (Vol, Tune,
  Overlap, A/D/S/R, Grain size, Density).
- **Playhead:** the red line tracks playback while a note sounds.

## Parameters

Master volume (index 0), then 13 parameters per layer L1–L3:

| Name | Range            | Notes                                   |
|------|------------------|-----------------------------------------|
| Vol  | 0–100 %          | layer volume                            |
| Tune | −100…+100 cents  | fine tune                               |
| Mode | OneShot/Fwd/Gran | loop mode                               |
| Play | From loop/start  | play head before entering the loop      |
| LpSt | 0–100 %          | loop start (fraction of sample)         |
| LpEn | 0–100 %          | loop end                                |
| Ovlp | 0–500 ms         | loop crossfade (Forward mode)           |
| Atk  | 1–2000 ms        | amp attack                              |
| Dec  | 1–2000 ms        | amp decay                               |
| Sus  | 0–100 %          | amp sustain                             |
| Rel  | 5–4000 ms        | amp release                             |
| GrSz | 5–500 ms         | grain size (Granular mode)              |
| GrDn | 1–100 grains/s   | grain density (Granular mode)           |

## Building locally (optional)

    sudo apt-get install -y g++-mingw-w64-x86-64 g++-mingw-w64-i686 wine64
    make            # both DLLs
    make test       # Wine smoke-test hosts
    make enginetest # native DSP unit test
