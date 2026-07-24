# Sampla — Granular Sampler + Sample Librarian

Two VST2 instruments for Ableton Live 9 on Windows 10, native Win32 GUIs,
built entirely on GitHub Actions — no local toolchain required. The CI
artifact contains x64 + x86 DLLs for both:

- **Granular Sampler** (`GranularSampler_*.dll`) — 3-layer granular sampler
  with a tempo-synced glitch sequencer. Sources in `src/`, docs below.
- **Sample Librarian** (`SampleLibrarian_*.dll`) — scans your sample library,
  spectrally fingerprints every WAV, and layers 2–4 files that match in tonal
  content but occupy different parts of the spectrum. Sources in `librarian/`.

## Sample Librarian

Point it at your library folder (click the path box), press **SCAN**, and it
walks every `.wav` recursively — filtered by **MinLen / MaxLen** (seconds) and
**MaxMB** (skip oversized files) — computing a spectral fingerprint per file:
a 16-band energy profile, a 12-bin chroma (tonal content), and a pitch
estimate with confidence (FFT autocorrelation). Results are cached in a
`.sample_librarian.idx` file at the library root, so rescanning a ~100k-file
library only analyzes new or changed files. Scanning runs on a background
thread with live progress; a large cold scan is a get-a-coffee affair, the
cached rescan takes seconds.

The library is drawn as a **constellation** (2-component PCA over the
fingerprints), so similar sounds cluster together. **RANDOMIZE** deals 12
combos into the palette; each combo picks a seed file plus 1–3 partners that
**match in chroma** but have **low spectral overlap** — layers that fit
tonally while occupying different frequency ranges. With **Tune to key** on,
partners are transposed (chroma rotation + exact-Hz refinement when pitch is
confident) so everything lands in the same key — which also widens the pool
of possible matches.

Click a combo button to open its **layer controls** (per-layer Vol / Pan /
Tune, the combo's constellation lights up in the map) and play it from a
**MIDI keyboard** — C4 is native pitch, monophonic, with Attack/Release.
**EXPORT WAV** renders the combo (layers mixed at their current knob
settings) to a new 16-bit WAV. **Original files are never modified.**

**Audition & console.** Click any star in the constellation to hear a
snippet of it (native pitch, no MIDI needed); its analysis — duration, pitch
with note name and confidence, spectral region — is logged to the scrolling
message console under the palette, alongside scan progress, combo loads,
exports and synthesis activity.

**Lasso + style synthesis.** Click-drag on the constellation to lasso a
region; the selected sounds become the *style* for two generators:

- **SYNTHESIZE** — the built-in, dependency-free path: draws a one-shot from
  a compact spectral-statistics model of the selection (per-bin
  log-magnitude mean/variance, temporally-smoothed stochastic resynthesis,
  stereo phase decorrelation, percussive envelope). Always available.
- **GENERATE NN** — the neural path (see below): encodes the lassoed audio
  through a **RAVE** model and decodes a fresh one-shot, shaped by
  **NLen** (length 0.3–6 s), **Chaos** (latent exploration temperature),
  **Morph** (blend two lassoed sources' latents), and **Sprd** (random
  pitch spread ±semitones). Available when a model is installed.

Both re-roll a new deterministic seed each press, audition immediately, and
export via **EXP SYNTH** / **EXP NN**.

### Neural sampler (RAVE via ONNX Runtime)

The spectral synth averages spectra, so it tends toward the same muddy wash;
the neural engine learns the *joint* time/frequency/phase structure, so its
one-shots keep transients and character. It runs a pretrained
[RAVE](https://github.com/acids-ircam/RAVE) model through **ONNX Runtime**,
which is **loaded dynamically at runtime** — the plugin has no link-time
dependency on it and runs fine without it (the NN panel just shows
`NN: absent`). Install by dropping these next to the plugin DLL:

    onnxruntime.dll        official ONNX Runtime for Windows (x64)
    rave_encoder.onnx      audio -> latent
    rave_decoder.onnx      latent -> audio
    rave_sr.txt            model sample rate

**Getting a model, all GitHub-side (no local toolchain, no GPU):** the
`convert-model` workflow (Actions tab -> Run workflow) takes the URL of a
pretrained RAVE `.ts` checkpoint and, on a plain CPU runner, exports the
ONNX pair as a downloadable artifact — see `tools/export_rave_onnx.py`.
*Converting* a trained checkpoint is CPU-only; *training* RAVE from scratch
needs a GPU (Colab/Kaggle/paid runner), but many checkpoints are published.
`tools/make_test_model.py` builds a tiny stand-in pair that CI uses to test
the whole encode -> perturb -> decode path.

Params (23, all automatable): Master, Atk, Rel, TuneKey, MinLen, MaxLen,
MaxMB, Vol/Pan/Tune for layers 1–4, then NLen/NChaos/NMorph/NSprd. The chunk
persists params, the library path, and all 12 combos with the selection.

---

# Granular Sampler

## Status: Milestone 4 — glitch sequencer + wav operators

- **Tempo-synced glitch engine.** A 16-step sequencer bar (classic trance
  grid, beat markers every 4 steps) follows the DAW transport via VST
  time-info (ppq + tempo), falling back to an internal clock when the host
  doesn't provide one. Each step picks one of **7 realtime loop-rearrange
  algorithms** — Stutter, Stutter-16, Reverse, Tapestop, Half-speed, trance
  Gate, and chaos-seeded slice Scramble — applied to the summed bus by
  snapshotting the previous step's audio and rearranging it live. Click a
  cell to cycle the algorithm, right-click to clear; the playing step is
  highlighted. Div (1/32…1/4) and Mix knobs sit beside the grid; the whole
  pattern is automatable (params St01–St16).
- **Wav operators per layer** (7 more knobs/controls): **Strch** —
  pitch-preserving time stretch 0.25–4× (grain-stream OLA); **Tonal** —
  spectral tonal/atonal balance (isolate peaks vs residual noise); **Tilt** —
  spectral dark/bright tilt; **Shft** — inharmonic spectral bin shift;
  **Frz** — spectral magnitude freeze (phases keep running → shimmer);
  plus the **Spec** mode button and **SAmt** intensity knob.
- **7 chaotic spectral artifact modes** (Spec button): Scramble (seed-driven
  bin swaps), Robot (phase collapse), Whisper (phase randomize), Holes
  (spectral dropouts), Mirror (spectrum fold), Crush (magnitude quantize),
  Smear (spectral trails). Randomness is steered by the chaos seed.
- **Fix:** with `effFlagsProgramChunks`, hosts persist only the chunk — knob
  positions were never saved. The chunk (SMPL4) now serializes all params
  alongside the sample/seed paths, so the full state survives a reload.

## Milestone 3 — experimental granular + click-free loops

Adds a deep experimental granular set and fixes loop smoothness on top of the
milestone-2 GUI sampler:

- **Seamless loops.** The Forward loop now uses a single read pointer with an
  equal-power (constant-power) crossfade, and the head→loop transition flows
  through the same fade — no hard jump at the seam even when the Overlap is
  larger than the loop, so there's no click. The crossfade region is shaded
  **amber** in the waveform so you can see exactly where it blends.
- **10 experimental granular controls per layer:** Spray (grain scatter),
  PJit (per-grain detune), Pan (stereo scatter), Rev (reverse-grain
  probability), Scan (drift the grain source through the loop), Shape (grain
  window skew), Bits (bit-crush), Deci (sample-rate reduction), Chaos
  (seed-driven bit rearranging), TJit (grain spawn-time jitter).
- **Chaos SEED (.txt).** Drop a text file on the top bar (or double-click the
  SEED box). Its bytes deterministically steer the "chaotic bit re-arranging"
  glitch driven by the per-layer **Chaos** knob — same seed + note reproduces
  the same mangling.

## Editor + engine (from milestone 2)

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
automatable VST parameter (70 in total). Sample **file paths** and the chaos
**seed path** are saved with the preset/Live set and reloaded on recall.

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

- **Load a sample:** drag a `.wav` onto one of the three lanes, or double-click
  a lane's waveform to open a file browser.
- **Load a chaos seed:** drag a `.txt` anywhere onto the window, or double-click
  the **CHAOS SEED** box in the top bar.
- **Loop points:** drag the two yellow handles in the waveform. The amber bands
  show the crossfade/overlap region.
- **Mode / Play:** click the **Mode** button to cycle OneShot → Forward →
  Granular; click the **From start / From loop** button to toggle.
- **Knobs:** drag any knob **vertically** to change its value.
- **Playhead:** the red line tracks playback while a note sounds.

## Parameters

109 total: Master (0), 30 per layer L1–L3 (1–90), glitch sequencer (91–108:
St01–St16 step algorithms, GDiv, GMix). Per-layer:

| Name  | Range            | Notes                                        |
|-------|------------------|----------------------------------------------|
| Vol   | 0–100 %          | layer volume                                 |
| Tune  | −100…+100 cents  | fine tune                                    |
| Mode  | OneShot/Fwd/Gran | loop mode                                    |
| Play  | From loop/start  | play head before entering the loop           |
| LpSt  | 0–100 %          | loop start (fraction of sample)              |
| LpEn  | 0–100 %          | loop end                                      |
| Ovlp  | 0–500 ms         | loop crossfade (Forward mode)                |
| Atk   | 1–2000 ms        | amp attack                                    |
| Dec   | 1–2000 ms        | amp decay                                     |
| Sus   | 0–100 %          | amp sustain                                   |
| Rel   | 5–4000 ms        | amp release                                   |
| GrSz  | 5–500 ms         | grain size (Granular mode)                    |
| GrDn  | 1–100 grains/s   | grain density (Granular mode)                 |
| Spray | 0–500 ms         | random grain-start scatter                    |
| PJit  | 0–12 st          | per-grain random detune                       |
| Pan   | 0–100 %          | per-grain stereo scatter                      |
| Rev   | 0–100 %          | probability a grain plays reversed            |
| Scan  | −100…+100 %      | drift the grain source through the loop       |
| Shape | 0–100 %          | grain window skew (50 % = symmetric)          |
| Bits  | 16–1 bit         | bit-crush depth (16 = clean)                  |
| Deci  | 1–50×            | sample-rate reduction (1 = off)               |
| Chaos | 0–100 %          | seed-driven chaotic bit rearranging           |
| TJit  | 0–100 %          | grain spawn-time jitter                       |
| Strch | 0.25–4×          | pitch-preserving time stretch                 |
| Tonal | −100…+100 %      | spectral tonal/atonal balance                 |
| Tilt  | −100…+100 %      | spectral dark/bright tilt                     |
| Shft  | −64…+64 bins     | inharmonic spectral shift                     |
| Frz   | 0–100 %          | spectral magnitude freeze                     |
| SMode | Off + 7 modes    | chaotic spectral artifact mode                |
| SAmt  | 0–100 %          | artifact-mode intensity                       |

## Building locally (optional)

    sudo apt-get install -y g++-mingw-w64-x86-64 g++-mingw-w64-i686 wine64
    make               # all four DLLs (both plugins, x64 + x86)
    make test          # Wine smoke-test hosts
    make enginetest    # sampler DSP unit test (native)
    make librariantest # librarian engine unit test (native)
