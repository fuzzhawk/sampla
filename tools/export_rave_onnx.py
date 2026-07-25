#!/usr/bin/env python3
"""Export a pretrained RAVE checkpoint to the ONNX encoder/decoder pair the
Sample Librarian's neural engine loads.

RAVE (https://github.com/acids-ircam/RAVE) ships models as scripted TorchScript
(.ts) files. This splits one into two ONNX graphs matching the plugin's
contract:

    rave_encoder.onnx   audio  [1, 1, N]  ->  latent [1, D, T]
    rave_decoder.onnx   latent [1, D, T]  ->  audio  [1, 1, N']
    rave_sr.txt         the model's sample rate

Everything is CPU-only — no GPU needed to export, so it runs on a plain GitHub
Actions runner (the .github/workflows/convert-model.yml workflow drives it).
Training a RAVE model from scratch does need a GPU; this script only converts
an already-trained checkpoint (many are published, e.g. the ACIDS releases).

Usage:
    pip install torch onnx
    python3 tools/export_rave_onnx.py <model.ts> <out_dir> [--sr 48000]
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint", help="RAVE .ts (scripted) checkpoint")
    ap.add_argument("outdir", help="directory to write the ONNX pair into")
    ap.add_argument("--sr", type=int, default=0,
                    help="model sample rate; read from the model if omitted")
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--frames", type=int, default=131072,
                    help="example audio length used to trace shapes")
    args = ap.parse_args()

    import torch

    os.makedirs(args.outdir, exist_ok=True)
    model = torch.jit.load(args.checkpoint, map_location="cpu").eval()

    sr = args.sr
    if sr == 0:
        for attr in ("sr", "sampling_rate", "sample_rate"):
            if hasattr(model, attr):
                try:
                    sr = int(getattr(model, attr))
                    break
                except Exception:
                    pass
    if sr == 0:
        sr = 48000
        print(f"warning: could not read sample rate, defaulting to {sr}")

    # RAVE scripted models expose encode() and decode(); wrap each so the ONNX
    # graph has a single audio-in / latent-out (and vice-versa) signature.
    class Encoder(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, audio):           # [1,1,N] -> [1,D,T]
            z = self.m.encode(audio)
            if isinstance(z, (tuple, list)):
                z = z[0]
            return z

    class Decoder(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, latent):          # [1,D,T] -> [1,1,N]
            y = self.m.decode(latent)
            if isinstance(y, (tuple, list)):
                y = y[0]
            return y

    enc = Encoder(model).eval()
    dec = Decoder(model).eval()

    # RAVE checkpoints are TorchScript ScriptModules; a call into a scripted
    # submodule can't be TRACED, so script the wrappers — then ONNX export uses
    # the scripted graph directly. Plain nn.Modules fall back to tracing.
    def scripted(wrapper):
        try:
            return torch.jit.script(wrapper)
        except Exception as e:
            print(f"  (scripting wrapper failed, will trace: {e})")
            return wrapper

    enc_s = scripted(enc)
    dec_s = scripted(dec)

    audio = torch.zeros(1, 1, args.frames)
    with torch.no_grad():
        z = enc_s(audio)
    print(f"latent shape from {args.frames} samples: {tuple(z.shape)} "
          f"(hop ~= {args.frames // max(1, z.shape[-1])} samples/frame)")

    enc_path = os.path.join(args.outdir, "rave_encoder.onnx")
    dec_path = os.path.join(args.outdir, "rave_decoder.onnx")

    def export(mod, example, path, in_name, out_name):
        # Force the legacy TorchScript exporter (dynamo=False): most reliable
        # for RAVE-style conv models, and it needs no onnxscript. Falls back
        # for older torch that lacks the `dynamo` kwarg.
        kw = dict(opset_version=args.opset, input_names=[in_name],
                  output_names=[out_name],
                  dynamic_axes={in_name: {2: "L"}, out_name: {2: "L2"}})
        try:
            torch.onnx.export(mod, example, path, dynamo=False, **kw)
        except TypeError:
            torch.onnx.export(mod, example, path, **kw)

    export(enc_s, audio, enc_path, "audio", "latent")
    export(dec_s, z, dec_path, "latent", "audio")

    with open(os.path.join(args.outdir, "rave_sr.txt"), "w") as f:
        f.write(f"{sr}\n")

    print(f"wrote:\n  {enc_path}\n  {dec_path}\n  "
          f"{os.path.join(args.outdir, 'rave_sr.txt')} ({sr} Hz)")
    print("Drop these (plus the matching onnxruntime.dll) next to the plugin.")


if __name__ == "__main__":
    try:
        main()
    except ImportError as e:
        print(f"missing dependency: {e}\n  pip install torch onnx", file=sys.stderr)
        sys.exit(2)
