#!/usr/bin/env python3
"""Train the Sample Librarian's neural one-shot model (a small spectral VAE)
and export RTNeural-format weights the plugin loads.

Trains on CPU in minutes on a few hundred coherent one-shots — no GPU. The
model learns a manifold of real log-magnitude STFT frames, so generation
draws plausible spectra (not the muddy average of the old spectral synth).

Pipeline:
    audio dir  ->  log-mag STFT frames (energy-gated)  ->  per-bin normalize
    ->  Dense VAE (encoder + decoder)  ->  nnvae.json (RTNeural-style layers
    + meta with spectral config and normalization stats)

The plugin needs only the DECODER to generate; the ENCODER is exported too so
a selected library sound can be re-imagined through the model.

Usage:
    pip install numpy torch soundfile   # soundfile optional (WAV works without)
    python3 tools/train_vae.py --input <audio_dir> --out nnvae.json \\
        [--sr 44100] [--epochs 60] [--max-files 1500] [--max-frames 80000]
"""
import argparse
import glob
import json
import os
import struct
import sys
import wave

import numpy as np

FFT = 1024
HOP = 256
NBINS = 192            # linear bins kept (~0..8.25 kHz @ 44.1k)
LATENT = 16
LOGEPS = 1e-4


# ----------------------------------------------------------------- audio io

def _load_wav_builtin(path, target_sr):
    """Minimal WAV reader (PCM 16/24/32 + float) so training works without
    soundfile. Returns mono float32 at target_sr (linear resample)."""
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), \
            w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        a = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif sw == 4:
        a = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    elif sw == 1:
        a = (np.frombuffer(raw, dtype="u1").astype(np.float32) - 128) / 128.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype="u1").reshape(-1, 3).astype(np.int32)
        v = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        v = np.where(v & 0x800000, v - (1 << 24), v)
        a = v.astype(np.float32) / 8388608.0
    else:
        return None
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1)
    return _resample(a, sr, target_sr)


def _resample(a, sr, target_sr):
    if sr == target_sr or len(a) < 2:
        return a
    n2 = int(len(a) * target_sr / sr)
    if n2 < 2:
        return a
    xp = np.linspace(0, len(a) - 1, n2)
    return np.interp(xp, np.arange(len(a)), a).astype(np.float32)


def load_audio(path, target_sr):
    try:
        import soundfile as sf
        a, sr = sf.read(path, dtype="float32", always_2d=True)
        a = a.mean(axis=1)
        return _resample(a, sr, target_sr)
    except Exception:
        if path.lower().endswith(".wav"):
            try:
                return _load_wav_builtin(path, target_sr)
            except Exception:
                return None
        return None


# ------------------------------------------------------------- features

def stft_logmag(x):
    win = np.hanning(FFT).astype(np.float32)
    if len(x) < FFT:
        x = np.pad(x, (0, FFT - len(x)))
    frames = []
    for s in range(0, len(x) - FFT + 1, HOP):
        seg = x[s:s + FFT] * win
        spec = np.fft.rfft(seg)
        mag = np.abs(spec)[:NBINS]
        frames.append(np.log(mag + LOGEPS))
    if not frames:
        return np.zeros((0, NBINS), np.float32)
    return np.asarray(frames, np.float32)


def collect_frames(files, target_sr, max_frames):
    out = []
    kept_files = 0
    for p in files:
        a = load_audio(p, target_sr)
        if a is None or len(a) < FFT:
            continue
        peak = np.max(np.abs(a))
        if peak < 1e-4:
            continue
        a = a / peak
        f = stft_logmag(a)
        if len(f) == 0:
            continue
        # energy gate: keep frames whose broadband level is within 40 dB of
        # the file's loudest frame, so silence/tails don't dominate
        e = f.mean(axis=1)
        thr = e.max() - 4.0
        f = f[e > thr]
        if len(f):
            out.append(f)
            kept_files += 1
        if sum(len(x) for x in out) >= max_frames:
            break
    if not out:
        return np.zeros((0, NBINS), np.float32), 0
    frames = np.concatenate(out, axis=0)
    if len(frames) > max_frames:
        idx = np.random.default_rng(0).choice(len(frames), max_frames, replace=False)
        frames = frames[idx]
    return frames, kept_files


# ------------------------------------------------------------------- model

def train(frames, epochs, seed):
    import torch
    import torch.nn as nn
    torch.manual_seed(seed)

    mean = frames.mean(axis=0)
    std = frames.std(axis=0) + 1e-3
    X = torch.from_numpy((frames - mean) / std).float()

    class VAE(nn.Module):
        def __init__(self):
            super().__init__()
            self.e1 = nn.Linear(NBINS, 256)
            self.e2 = nn.Linear(256, 128)
            self.mu = nn.Linear(128, LATENT)
            self.lv = nn.Linear(128, LATENT)
            self.d1 = nn.Linear(LATENT, 128)
            self.d2 = nn.Linear(128, 256)
            self.d3 = nn.Linear(256, NBINS)
            self.act = nn.Tanh()

        def encode(self, x):
            h = self.act(self.e2(self.act(self.e1(x))))
            return self.mu(h), self.lv(h)

        def decode(self, z):
            return self.d3(self.act(self.d2(self.act(self.d1(z)))))

        def forward(self, x):
            mu, lv = self.encode(x)
            z = mu + torch.randn_like(mu) * torch.exp(0.5 * lv)
            return self.decode(z), mu, lv

    net = VAE()
    opt = torch.optim.Adam(net.parameters(), lr=1e-3)
    n = X.shape[0]
    bs = 256
    beta = 0.001
    for ep in range(epochs):
        perm = torch.randperm(n)
        tot = 0.0
        for i in range(0, n, bs):
            xb = X[perm[i:i + bs]]
            recon, mu, lv = net(xb)
            rl = ((recon - xb) ** 2).mean()
            kl = (-0.5 * (1 + lv - mu ** 2 - lv.exp()).mean())
            loss = rl + beta * kl
            opt.zero_grad()
            loss.backward()
            opt.step()
            tot += float(loss) * xb.shape[0]
        if ep % 5 == 0 or ep == epochs - 1:
            print(f"  epoch {ep:3d}  loss {tot / n:.4f}")

    with torch.no_grad():
        mu_all, _ = net.encode(X)
        lat_mean = mu_all.mean(axis=0).numpy()
        lat_std = mu_all.std(axis=0).numpy() + 1e-3
    return net, mean, std, lat_mean, lat_std


# ------------------------------------------------------------------- export

def dense(layer, activation):
    """RTNeural-style dense layer: weights[out][in], bias[out]."""
    W = layer.weight.detach().numpy()      # torch stores [out][in]
    b = layer.bias.detach().numpy()
    return {"type": "dense", "activation": activation,
            "out": int(W.shape[0]),
            "weights": [[float(v) for v in row] for row in W],
            "bias": [float(v) for v in b]}


def export(path, net, mean, std, lat_mean, lat_std, sr):
    model = {
        "format": "rtneural-vae-1",
        "meta": {
            "sr": int(sr), "fft": FFT, "hop": HOP, "nbins": NBINS,
            "latent": LATENT, "logeps": LOGEPS,
            "mean": [float(v) for v in mean],
            "std": [float(v) for v in std],
            "lat_mean": [float(v) for v in lat_mean],
            "lat_std": [float(v) for v in lat_std],
        },
        "encoder": {"in": NBINS, "layers": [
            dense(net.e1, "tanh"), dense(net.e2, "tanh"),
            dense(net.mu, "linear"),      # encoder outputs the latent mean
        ]},
        "decoder": {"in": LATENT, "layers": [
            dense(net.d1, "tanh"), dense(net.d2, "tanh"),
            dense(net.d3, "linear"),
        ]},
    }
    with open(path, "w") as f:
        json.dump(model, f)
    print(f"wrote {path}  ({os.path.getsize(path) / 1e6:.1f} MB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="folder of audio one-shots")
    ap.add_argument("--out", default="nnvae.json")
    ap.add_argument("--sr", type=int, default=44100)
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--max-files", type=int, default=1500)
    ap.add_argument("--max-frames", type=int, default=80000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    exts = ("wav", "WAV", "aif", "aiff", "AIFF", "flac", "FLAC", "mp3", "MP3", "ogg")
    files = []
    for e in exts:
        files += glob.glob(os.path.join(args.input, "**", f"*.{e}"), recursive=True)
    files = sorted(set(files))[: args.max_files]
    print(f"found {len(files)} audio files under {args.input}")
    if not files:
        print("no audio found", file=sys.stderr)
        sys.exit(2)

    frames, kept = collect_frames(files, args.sr, args.max_frames)
    print(f"collected {len(frames)} frames from {kept} files")
    if len(frames) < 200:
        print("not enough usable frames — add more/longer samples", file=sys.stderr)
        sys.exit(2)

    net, mean, std, lat_mean, lat_std = train(frames, args.epochs, args.seed)
    export(args.out, net, mean, std, lat_mean, lat_std, args.sr)


if __name__ == "__main__":
    main()
