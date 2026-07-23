# Granular Sampler

GUI-less VST2 granular sampler instrument for Ableton Live 9 on Windows 10.
Built entirely on GitHub Actions — no local toolchain required.

## Status: Milestone 1

Loads as an Instrument, exposes 8 automatable parameters, receives MIDI, and
plays a sine test voice with attack/release so the full MIDI → audio path can
be verified in Live. The granular engine replaces `renderVoice()` in
`src/plugin.cpp` next.

## Layout

    src/vst2.h                    minimal VST2 ABI header (only dependency)
    src/plugin.cpp                the entire plugin
    test/host.c                   console host used by the CI smoke test
    Makefile                      builds x64 + x86 DLLs and test hosts
    .github/workflows/build.yml   compile, Wine smoke test, artifacts

## Browser-only workflow

1. Commit any change through the GitHub web editor (or press `.` for
   github.dev).
2. The `build` workflow compiles both DLLs and runs the 64-bit one under Wine
   in a minimal host, verifying it loads and answers the VST2 dispatcher.
3. Green check → download `GranularSampler-dlls` from the run's page under
   the Actions tab.
4. Copy the DLL matching your Live install (x64 for 64-bit Live) into the
   folder set in Live: Preferences → File/Folder → Plug-In Sources.
5. Live scans it on rescan/restart; it appears under Plug-ins as an
   Instrument. Drop it on a MIDI track and play.

For keeper builds: Releases → Draft a new release → publish. The workflow
attaches both DLLs to the release automatically.

## Params

| # | Name      | Range          | Active in M1        |
|---|-----------|----------------|---------------------|
| 0 | GrainSize | 5–500 ms       | no (engine pending) |
| 1 | Density   | 1–100 grains/s | no                  |
| 2 | Position  | 0–100 %        | no                  |
| 3 | Spray     | 0–250 ms       | no                  |
| 4 | Pitch     | −24…+24 st     | yes                 |
| 5 | Attack    | 1–2000 ms      | yes                 |
| 6 | Release   | 5–4000 ms      | yes                 |
| 7 | Mix       | 0–100 %        | yes                 |
