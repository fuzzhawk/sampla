#!/usr/bin/env python3
"""Emit a tiny random-weight nnvae.json in the trainer's RTNeural-format, so CI
can exercise the plugin's VAE load + generate path without PyTorch. Output is
noise (random weights) — it only proves the pipeline runs. numpy only.

Usage: python3 tools/make_test_vae.py <out_dir>
"""
import json
import os
import sys
import numpy as np

NBINS, LATENT, FFT, HOP = 192, 16, 1024, 256


def dense(nin, nout, act, rng):
    W = rng.standard_normal((nout, nin)) / np.sqrt(nin)
    b = rng.standard_normal(nout) * 0.01
    return {"type": "dense", "activation": act, "out": nout,
            "weights": [[float(v) for v in row] for row in W],
            "bias": [float(v) for v in b]}


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/nnvae"
    os.makedirs(outdir, exist_ok=True)
    rng = np.random.default_rng(3)
    model = {
        "format": "rtneural-vae-1",
        "meta": {
            "sr": 44100, "fft": FFT, "hop": HOP, "nbins": NBINS,
            "latent": LATENT, "logeps": 1e-4,
            "mean": [float(x) for x in (rng.standard_normal(NBINS) - 4.0)],
            "std": [1.0] * NBINS,
            "lat_mean": [0.0] * LATENT,
            "lat_std": [1.0] * LATENT,
        },
        "encoder": {"in": NBINS, "layers": [
            dense(NBINS, 256, "tanh", rng), dense(256, 128, "tanh", rng),
            dense(128, LATENT, "linear", rng)]},
        "decoder": {"in": LATENT, "layers": [
            dense(LATENT, 128, "tanh", rng), dense(128, 256, "tanh", rng),
            dense(256, NBINS, "linear", rng)]},
    }
    path = os.path.join(outdir, "nnvae.json")
    with open(path, "w") as f:
        json.dump(model, f)
    print(f"wrote test VAE -> {path} ({os.path.getsize(path)/1e6:.1f} MB)")


if __name__ == "__main__":
    main()
