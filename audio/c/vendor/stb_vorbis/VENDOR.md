# stb_vorbis, vendored

- Source: https://github.com/sezero/stb, branch `stb_vorbis-sezero`, `stb_vorbis.c` at commit
  `dd0c5eccaf092012b531d69d595fb587ece79571` (stb_vorbis v1.22 and the fixes since), unchanged.
- Why this fork and not https://github.com/nothings/stb: the original has taken no fix to
  `stb_vorbis.c` since v1.22, while CVE-2023-45675..45682 and CVE-2026-5316/5317/89266 stayed open.
  This branch, kept by SDL_mixer's and SDL_sound's maintainer and followed by both and by libxmp,
  carries those fixes and about thirty UB, leak and bounds fixes.
- License: public domain or MIT, the reader's choice (the notice at the end of the file).
- SHA-256: `6f4805310469cd087e771c30866d5259a0725905a8eff06cfcb406c0ce41e68c`.
- Built through `../../audio_engine.c` with `STB_VORBIS_NO_STDIO` and
  `STB_VORBIS_NO_PUSHDATA_API`, on every target but the two wasm builds (it needs libm, which
  loft's freestanding wasm build has not).
