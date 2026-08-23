# Sampla — Granular Sampler + Sample Librarian + Match Slicer + Spectral Canvas + Spectral Split

Five VST2 plugins for Ableton Live 9 on Windows 10, native Win32 GUIs, built
entirely on GitHub Actions — no local toolchain required. The three instruments
plus the Spectral Canvas effect share a pink "kawaii" theme; Spectral Split has
its own minimal dark theme. The CI artifact contains x64 + x86 DLLs for all
five (three instruments + two insert effects):

- **Granular Sampler** (`GranularSampler_*.dll`) — 3-layer granular sampler
  with a tempo-synced glitch sequencer. Sources in `src/`, docs below.
- **Sample Librarian** (`SampleLibrarian_*.dll`) — scans your sample library,
  spectrally fingerprints every WAV, and layers 2–4 files that match in tonal
  content but occupy different parts of the spectrum. Sources in `librarian/`.
- **Match Slicer** (`MatchSlicer_*.dll`) — tempo-synced audio mosaicing: load a
  guide (an amen break, a synth melody) and a main file; it slices the guide,
  finds the best-matching section of the main file for each slice, and plays
  them back in the guide's groove, locked to the DAW. Sources in `slicematch/`.
- **Spectral Canvas** (`SpectralCanvas_*.dll`) — an INSERT EFFECT (not an
  instrument): captures an N-bar loop of the incoming audio, turns it into an
  editable spectrogram you paint on (move / pitch / smear / cellular-automata /
  gain / erase), and re-renders it every loop. Sources in `spectral/`.
- **Spectral Split** (`SpectralSplit_*.dll`) — a single-purpose INSERT EFFECT:
  one big knob crossfades between the tonal (sustained/harmonic) and atonal
  (broadband/transient) content of the incoming audio, via a high-resolution
  4096-point STFT. Sources in `split/`.

## Match Slicer

Drop a **GUIDE** file (the rhythm/structure you want — an amen break, a synth
melody) and a **MAIN** file (the sound you want to hear) onto their lanes, then
press **MATCH**. The engine cuts the guide into slices — either a tempo **Grid**
(Div 1/4…1/32 × Bars) or the guide's own **transients** — fingerprints each
slice (16 log-bands + centroid + loudness), searches the main file for the
window whose fingerprint matches best, and rearranges those main-file chunks
onto the guide's timeline. Playback is **locked to the host transport** (it
follows tempo and bar position), so it stays in the groove; a held MIDI note
also triggers free-run playback.

**Transient detection, both lanes.** Two independent slice modes with their own
sensitivity: **GuideSlc** switches the guide between grid and transient slicing,
and **MainSlc** does the same for how the main file is chopped into match
candidates — in transient mode the candidate windows start on the main file's
own onsets, so a matched chunk keeps its natural attack instead of snapping to a
grid, freeing you from the tempo grid for micro-timing inside the loop. A
spectral-flux onset detector drives both; the **GuideThr** / **MainThr** knobs
set each threshold (higher = only the strongest transients). The main lane draws
mint tick marks at every detected main-file onset when MainSlc is on.

The waveforms show the guide's slice boundaries and which regions of the main
file got used; the **map** strip colors each guide slice by its source. Knobs:
**Mix** (guide↔mosaic), **Div**/**Bars** (grid), **Xfade** (slice crossfade),
**Var** (anti-repeat variety), **SpecW** (timbre vs loudness weighting),
**GuideThr**/**MainThr** (onset thresholds), plus **GainFollow** (match the
guide slice's loudness), **PitchMatch** (repitch the main chunk toward the guide
slice's pitch — for melodic guides), and **StretchFit** (resample the main chunk
to fill the slot). **EXPORT WAV** renders the mosaic; originals are never
modified. 14 automatable params; the chunk saves both file paths and re-matches
on reload.

## Spectral Canvas

An **insert effect** in the vein of Gross Beat / Stutter Edit, but the control
surface is a **spectrogram you paint on**. Drop it on an audio track; it
continuously captures an **N-bar loop** (1 / 2 / 4 / 8, locked to the host
tempo + time signature) and turns it into an editable time × frequency canvas.
What you hear is the *painted* version, re-rendered every loop. Because the
render happens once per loop on a background thread, it uses **Griffin-Lim**
phase reconstruction — quality a real-time effect can't afford.

Like Gross Beat, it reaches back in time: the wet output is the previous loop's
audio re-arranged through your edits, so there's an inherent one-loop latency
(the first loop after you hit play passes dry). Two capture modes: **streaming**
(the edits ride on whatever's playing, refreshed each loop) and **Freeze** (grab
one loop and keep painting against that fixed image — it re-renders the instant
you change an edit).

Paint tools (drag a region on the canvas; for most tools the op applies on
release, for **Move** you drag the selection to its new home):

- **Move** — copy or cut a rectangle of the spectrogram and paste it at a new
  time/frequency. Time-moves wrap around the loop; this is the "rearrange
  sections in time" gesture.
- **Pitch** — resample a region's frequency axis by a semitone amount (a pitch
  bend on just that patch of spectrum).
- **Smear** — a freeze-tail (sustain energy forward in time) or a spectral blur.
- **CA** — a cellular-automaton gate: a coarse freq-band × time-step grid seeded
  from the audio, evolving with a Wolfram rule, gating the magnitudes into
  animated, tempo-locked glitch patterns.
- **Gain / Erase** — boost, duck, or silence a region.

Edits are a **non-destructive op stack** (Undo / Clear, drawn as tinted overlays
on the canvas) that re-applies every loop and serializes into the plugin chunk,
so a painted canvas is saved with the project. Knobs: **Mix** (dry↔wet),
**Master**, **Amt** / **Pitch** / **Rule** (tool parameters), **Qual**
(Griffin-Lim iterations: Lo/Md/Hi). The engine (`spectral/src/canvas.h`) is
headless and CI-tested; the Win32 canvas GUI is in `spectral/src/editor.h`.

## Spectral Split

A dead-simple, single-purpose **insert effect**: one big knob that crossfades
between the **tonal** and **atonal** halves of whatever you feed it. Drop it on
a track and dial toward TONAL to keep the sustained, harmonic, pitched content
(and drop the noise/transients), or toward ATONAL to keep the breath, air,
transients and texture (and drop the pitched tone). Dead centre is a
full-range, bit-exact bypass.

Under the hood it runs a **high-resolution 4096-point STFT at 4× overlap**
(Hann analysis + synthesis, constant-overlap-add) for the best frequency
resolution / fidelity, and separates each frame with median-based harmonic-
percussive separation (Fitzgerald 2010): the **tonal** estimate is a short
causal median across time per bin (sustained energy survives, one-frame
transients don't — and it adds no extra latency), the **atonal** estimate is a
median across frequency within the frame (broadband energy survives, narrow
tonal peaks are suppressed). Complementary Wiener masks (Mh + Mp = 1) guarantee
the two halves sum back to the original, so centre is a true bypass. The window
latency is reported to the host for delay compensation, and the dry path is
delay-matched so **Mix** stays phase-coherent.

Controls: the large **Split** knob (its arc shifts warm-amber on the atonal
side to cool-cyan on the tonal side; double-click to recentre), plus small
**Mix** (dry/wet) and **Output** knobs. Three automatable params, lightweight
and real-time. The engine (`split/src/split.h`) is headless and CI-tested; the
dark Win32 GUI is in `split/src/editor.h`.

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

### Neural sampler — two backends

`GENERATE NN` picks the best model present:

1. **Spectral VAE (RTNeural-format, primary, no runtime dependency).** A small
   variational autoencoder **you train on your own samples** — learns a
   manifold of real log-magnitude STFT frames, so generation draws plausible
   spectra instead of the averaging synth's mush. Weights load from
   `nnvae.json` (a plain Dense stack in the RTNeural JSON format, parsed with
   the vendored nlohmann/json and run directly — **header-only, nothing extra
   to ship**). Reconstruction is Griffin-Lim. Trains on **CPU in minutes, no
   GPU** — the whole reason it exists alongside RAVE.
   - **Train it:** Actions tab -> `train-vae` -> paste a URL to a `.zip` of
     one-shots -> download the `nnvae-model` artifact -> drop `nnvae.json` next
     to the plugin DLL. See `tools/train_vae.py`. A few hundred coherent
     one-shots is plenty; tighter and more coherent beats bigger.
   - **Hosting the zip:** any direct-download URL works — the runner fetches
     it. A **Google Drive** ("Anyone with the link") or **Dropbox** share URL
     is easiest (the workflow handles both). A GitHub Release asset also works,
     but use the *"Attach binaries"* dropzone on the Draft-release page (2 GB);
     the release-notes text box and the repo file uploader cap at 25 MB.
   - Controls: **NLen** (length), **Chaos** (latent temperature/drift),
     **Morph** (blend a lassoed library sound's latent in as a seed), **Sprd**
     (random pitch spread). Sculpt row: **Shape** (Pluck / Pad / Drone / Free
     — envelope + latent-motion presets, so one model makes keyboard plucks,
     sweeping pads, or sustained drones), **Key** (spectral pitch correction:
     snaps the fundamental to a chosen note, Off + C…B), **Tone** (dark↔bright
     tilt), **Motion** (sweeping band emphasis for pads), **Focus** (harmonic
     sharpening for a more melodic/less-muddy tone). Status reads `NN: VAE`.

2. **RAVE via ONNX Runtime (fallback, higher-fidelity, needs a GPU to train).**

The spectral synth averages spectra, so it tends toward the same muddy wash;
the neural engines learn structure, so their one-shots keep character. RAVE
runs a pretrained
[RAVE](https://github.com/acids-ircam/RAVE) model through **ONNX Runtime**,
which is **loaded dynamically at runtime** — the plugin has no link-time
dependency on it and runs fine without it (the NN panel just shows
`NN: absent`). Install by dropping these next to the plugin DLL:

    onnxruntime.dll        official ONNX Runtime for Windows (x64)
    rave_encoder.onnx      audio -> latent
    rave_decoder.onnx      latent -> audio
    rave_sr.txt            model sample rate

**Getting a model — two paths, no local toolchain either way:**

1. **Train on your own samples (Colab GPU):** open
   [`tools/train_rave_colab.ipynb`](tools/train_rave_colab.ipynb) in Google
   Colab, pick a GPU runtime, upload a zip of one-shots (one coherent
   category works best — all kicks, all vox, all pads), and it preprocesses,
   fine-tunes/trains a RAVE, exports TorchScript, converts to the ONNX pair,
   and hands you a downloadable zip. A few hundred one-shots is enough to
   fine-tune; training from scratch wants more (30 min–hours of audio).
2. **Convert an existing checkpoint (CPU, GitHub-side):** the `convert-model`
   workflow (Actions tab -> Run workflow) takes a pretrained RAVE `.ts` URL
   and exports the ONNX pair as a downloadable artifact on a plain CPU runner
   — see `tools/export_rave_onnx.py`.

The converter scripts thin encode/decode wrappers so ONNX export uses RAVE's
own graph (not a fragile trace) and forces the legacy exporter. The whole
chain — scripted `.ts` -> converter -> ONNX -> the plugin's ORT inference —
is verified in CI at real RAVE scale (D=16, hop~2048) via
`tools/make_test_model.py`, which also builds the tiny stand-in pair the
neural unit tests run against. *Training* RAVE needs a GPU (the Colab
notebook); *converting* a trained checkpoint is CPU-only.

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
    src/fft.h                     clean-room radix-2 FFT (shared by all plugins)
    librarian/src/                Sample Librarian: librarian.h + plugin.cpp + editor.h
    slicematch/src/               Match Slicer: slicer.h + plugin.cpp + editor.h
    spectral/src/canvas.h         Spectral Canvas engine (STFT, op stack, Griffin-Lim)
    spectral/src/plugin.cpp       Spectral Canvas VST effect (capture + render worker)
    spectral/src/editor.h         Spectral Canvas GUI (spectrogram + paint tools)
    split/src/split.h             Spectral Split engine (streaming HPSS, tonal/atonal)
    split/src/plugin.cpp          Spectral Split VST effect (real-time insert)
    split/src/editor.h            Spectral Split GUI (dark theme, big Split knob)
    test/host.c                   console host for the CI smoke test
    test/engine_test.cpp          native DSP unit test (real audio path)
    spectral/test/canvas_test.cpp native canvas engine unit test
    split/test/split_test.cpp     native split engine unit test
    Makefile                      builds x64 + x86 DLLs, hosts, engine tests
    .github/workflows/build.yml   compile, unit tests, Wine smoke test, artifacts

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
