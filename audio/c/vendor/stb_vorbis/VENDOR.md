# stb_vorbis, vendored

- Source: https://github.com/nothings/stb, `stb_vorbis.c` at commit
  `6b5799bae8a1fe3792691ef4b72ae26434c3e799` (stb_vorbis v1.25: v1.23's fixes, the 64-bit
  handling, and the granule-position limits and bad-file checks of 56438ac59, 903c869eb and
  98bc64014), with one one-line local change (see *Local change* below).
- Why the original again, not the `sezero/stb` fork we carried through 0.2.1: the original has
  woken up.  Since 2026-10-26 it has taken v1.23 — a burst of overflow, out-of-bounds, UB and
  decode fixes to `stb_vorbis.c` (codebook-size limit, `set_file_offset` overflow, type-2
  residue for multi-channel Vorbis, `lookup1_values` overflow, sample-clamp UB, floor1 decode,
  blocksizes below 256, untrusted page boundaries) — and the 64-bit seek/offset rework.  The
  dormant `sezero` branch (last `stb_vorbis.c` commit 2026-07-13) now LAGS that security batch,
  so following it would mean shipping known defects a release carries.  The original carries all
  of those fixes; it is missing only CVE-2023-45682, which we keep as a one-line local change.
- Local change (`modified` in loft.toml): the one-line guard of upstream PR nothings/stb#1560
  (GHSL-2023-172 / CVE-2023-45682) in the `DECODE` macro —
  `if (c->sparse && var >= 0) var = c->sorted_values[var];`.  Without it a crafted stream whose
  sparse codebook has no present entry reads `sorted_values[-1]` through a NULL base (a wild
  read / crash).  The fix is submitted upstream (PR #1560, still open); drop it once a tagged
  release carries it.
- License: public domain or MIT, the reader's choice (the notice at the end of the file).
- SHA-256: `711a7f0e1bac5eddba1d85c64b2dfa96851609e604ea6ec4f5c41ff3e724eaf0`.
- Built through `../../audio_engine.c` with `STB_VORBIS_NO_STDIO` and
  `STB_VORBIS_NO_PUSHDATA_API`, on every target but the two wasm builds (it needs libm, which
  loft's freestanding wasm build has not).
