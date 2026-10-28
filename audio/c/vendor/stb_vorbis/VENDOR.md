# stb_vorbis, vendored

- Source: https://github.com/nothings/stb, `stb_vorbis.c` at commit
  `2c980bb59875b0d32144a71867fbdebb2f77cd20` (stb_vorbis v1.22), unchanged.
- License: public domain or MIT, the reader's choice (the notice at the end of the file).
- SHA-256: `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`.
- Built through `../../audio_engine.c` with `STB_VORBIS_NO_STDIO` and
  `STB_VORBIS_NO_PUSHDATA_API`, on every target but the two wasm builds (it needs libm, which
  loft's freestanding wasm build has not).
