#!/usr/bin/env python3
"""Build a tiny RAVE-shaped ONNX encoder/decoder pair for CI and plumbing
tests. Same I/O contract as a real exported RAVE model — encoder maps audio
[1,1,N] to latent [1,D,T], decoder maps latent back to audio [1,1,N'] — but
the weights are just fixed pseudo-random projections, so it needs no torch,
downloads nothing, and builds in under a second.

Usage: python3 tools/make_test_model.py <output_dir>
"""
import sys
import os
import numpy as np
from onnx import helper, TensorProto, save

D = 8          # latent dims
HOP = 512      # audio samples per latent frame
K = 1024       # conv kernel


def tensor(name, arr):
    return helper.make_tensor(name, TensorProto.FLOAT, arr.shape,
                              arr.astype(np.float32).tobytes(), raw=True)


def build_encoder():
    rng = np.random.default_rng(1234)
    w = (rng.standard_normal((D, 1, K)) / np.sqrt(K)).astype(np.float32)
    conv = helper.make_node(
        "Conv", ["audio", "encW"], ["latent"],
        strides=[HOP], pads=[K // 2, K // 2 - 1], kernel_shape=[K])
    graph = helper.make_graph(
        [conv], "test_rave_encoder",
        [helper.make_tensor_value_info("audio", TensorProto.FLOAT,
                                       [1, 1, "N"])],
        [helper.make_tensor_value_info("latent", TensorProto.FLOAT,
                                       [1, D, "T"])],
        [tensor("encW", w)])
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)],
                             ir_version=8)


def build_decoder():
    rng = np.random.default_rng(5678)
    w = (rng.standard_normal((D, 1, K)) / np.sqrt(K * D)).astype(np.float32)
    up = helper.make_node(
        "ConvTranspose", ["latent", "decW"], ["raw"],
        strides=[HOP], pads=[K // 2, K // 2 - HOP], kernel_shape=[K])
    tanh = helper.make_node("Tanh", ["raw"], ["audio"])
    graph = helper.make_graph(
        [up, tanh], "test_rave_decoder",
        [helper.make_tensor_value_info("latent", TensorProto.FLOAT,
                                       [1, D, "T"])],
        [helper.make_tensor_value_info("audio", TensorProto.FLOAT,
                                       [1, 1, "N"])],
        [tensor("decW", w)])
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)],
                             ir_version=8)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/nn"
    os.makedirs(outdir, exist_ok=True)
    save(build_encoder(), os.path.join(outdir, "rave_encoder.onnx"))
    save(build_decoder(), os.path.join(outdir, "rave_decoder.onnx"))
    with open(os.path.join(outdir, "rave_sr.txt"), "w") as f:
        f.write("44100\n")
    print(f"test model pair written to {outdir} (D={D}, hop={HOP})")


if __name__ == "__main__":
    main()
