# third_party

## onnxruntime_c_api.h

The ONNX Runtime C API header, vendored from
[microsoft/onnxruntime](https://github.com/microsoft/onnxruntime) tag
**v1.17.3**, unmodified.

- License: **MIT** (Copyright (c) Microsoft Corporation).
- Only the header is vendored. The Sample Librarian loads the ONNX Runtime
  shared library **dynamically at runtime** (`LoadLibrary` / `dlopen`), so
  there is no build- or link-time dependency on ONNX Runtime — the plugin
  compiles and runs without it, and the neural panel simply reports itself
  unavailable when the runtime and models aren't present next to the plugin.
- MinGW compatibility SAL shims live in `librarian/src/neural.h`, not in this
  header, so the vendored copy stays byte-for-byte upstream and verifiable.

The runtime library itself (`onnxruntime.dll` on Windows,
`libonnxruntime.so` in CI) is **not** vendored; it is downloaded from the
official ONNX Runtime releases where needed.

## json.hpp

nlohmann/json single-header, v3.11.3, unmodified.

- License: **MIT** (Copyright (c) 2013-2023 Niels Lohmann).
- Used by `librarian/src/nnsynth.h` to parse the RTNeural-format weight JSON
  (`nnvae.json`) produced by `tools/train_vae.py`. It is the same JSON parser
  the RTNeural library itself uses, so the exported model is portable to
  RTNeural; here the small Dense stack is run directly (header-only, no
  runtime library to ship).
