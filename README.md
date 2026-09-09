# mtnbli — multi-threaded, SIMD-free NBLI / fNBLI codec

`mtnbli` is a drop-in, multi-threaded replacement for the two codecs in
[WangXuan95/NBLI](https://github.com/WangXuan95/NBLI) (v0.4, GPLv3). It

* decodes **every** NBLI and fNBLI stream — including the fNBLI `is_large=1` streams that the
  upstream `fNBLI.exe` **refuses** to touch unless the CPU has AVX2,
* needs **no** SSE4.1, no SSE4.2, no AVX, no AVX2 (and no SSSE3, BMI, FMA, POPCNT or LZCNT
  either) — the whole 16-lane AVX2 wave-front coder is re-implemented in plain integer C++,
* scales with the number of cores: by default **all** hardware threads of the machine are used.

It was written for machines like the **AMD Phenom II X6 1055T** (x86-64, MMX, 3DNow!, SSE, SSE2,
SSE3, SSE4a, ABM — but *no* SSE4.1/4.2, *no* AVX/AVX2), i.e. for exactly the "bucket" that modern
codecs leave behind.

---

## 1. Build

```bash
make            # portable ISA build  (default)
make native     # -march=native build, for benchmarking only -- NOT portable
make test       # run the full self test suite
make isa        # verify the binary contains no forbidden instruction
```

Only `g++`/`gcc` and `make` are needed (C++11). No external libraries: the PNG reader
(uPNG) and the PNM reader are vendored.

`make isa` disassembles the binary and greps the mnemonic list for anything belonging to
SSSE3 / SSE4.1 / SSE4.2 / AVX / AVX2 / BMI / BMI2 / FMA / POPCNT / LZCNT / AES / PCLMUL /
MOVBE / CRC32. The default build passes it — the highest SIMD level it uses is baseline SSE2.

## 2. Usage

```
mtnbli [-switches]  <in1> [-o <out1>]  [<in2> [-o <out2>]]  ...

To compress:     <in>  .pgm .ppm .pnm .png
                 <out> .fnbli .nbli .tnbli      (derived from <in> if not given)
To decompress:   <in>  .fnbli .nbli .tnbli
                 <out> .pgm .ppm .pnm .png

  -v          verbose
  -f          force overwrite of an existing output file
  -x          store a CRC32 in the stream when compressing (recommended)
  -t <N>      number of threads            (default: all hardware threads)
  -T <N>      number of tiles              (default 1 = a plain, untiled .fnbli/.nbli)
              -T 0 = auto (2 tiles per thread).  N > 1 produces a .tnbli container
  -N          compress with NBLI instead of fNBLI
  -g          NBLI: Golomb code tree instead of rANS   (slower, slightly smaller)
  -a          NBLI: advanced predictor                 (much slower, smaller)
  -0 .. -7    NBLI: distortion level, 0 = lossless (default), 1..7 = near-lossless
  --pnm       when decompressing, write PNM instead of PNG
```

Examples

```bash
mtnbli -x -T 0 photo.png  -o photo.tnbli    # fNBLI, tiled, 2 tiles per thread, CRC32
mtnbli -f --pnm photo.tnbli -o out.ppm      # decode it with every core
mtnbli -N -a -x big.ppm                     # NBLI + advanced predictor, single stream
mtnbli -f -t 6 *.fnbli *.nbli               # decode a batch, 6 threads
```

The exit status is the number of files that failed.

## 3. Why a new container (`.tnbli`)?

A `.nbli`/`.fnbli` file is **one** adaptive rANS/Golomb stream. Symbol *N* can only be decoded
once the complete coder state produced by symbols *0..N-1* — rANS state *and* every adaptive
context-model state — is known. That is inherently sequential: **no amount of threads can split a
single legacy file**, and no decoder can change that without changing the format.

So `mtnbli` adds a tiled container. It stores *K* **independent** NBLI/fNBLI streams, one per
horizontal strip of the image:

```
TnbliHeader           64 bytes   ("MTNBLI\1\0", width, height, n_tiles, crc32, codec, flags, near)
uint64 offset[n+1]    8*(n+1)    (8-byte aligned)
tile 0 blob ...                  a complete, self-contained .nbli or .fnbli stream
...
tile n-1 blob
```

Each blob has its own header and its own CRC, therefore

* **every strip decodes standalone with any legacy `NBLI` / `fNBLI` binary** — `.tnbli` files are
  not a lock-in, they are a concatenation of ordinary streams (see `tests/tnbli_split.py`), and
* the strips can be decoded (and encoded) **concurrently** — one image now scales with the core
  count.

`tests/tiles_check.py` verifies exactly that: it splits a `.tnbli` into its tiles and feeds each
one to the *upstream* decoder.

Tiling changes the compression ratio a little, because each strip restarts its model from scratch
— but also because the strips get different heights, which shifts where the 16-lane wave-front
coder kicks in. Measured on the 6000×4000 image above (untiled baseline: 11 739 126 bytes):

| tiles | size | Δ |
|---:|---:|---:|
| 1 (plain `.fnbli`) | 11 739 126 | — |
| 6  | 11 731 924 | **−0.06 %** |
| 16 | 11 758 142 | +0.16 % |
| 64 | 11 919 122 | +1.53 % |

Up to about 16 tiles the cost is negligible. `-T 0` (auto) picks `2 × n_threads`, clamped so that
no strip is shorter than 96 rows.

## 4. Portable AVX2 emulation

The fNBLI encoder uses a 16-lane diagonal wave-front coder when the image is large
(`is_large = 1` in the header) — **but only if the CPU has AVX2**; otherwise it falls back to a
slower 1-lane coder and sets `is_large = 0`. At decode time the upstream tool simply bails out on
`is_large = 1` files when the CPU lacks AVX2 — silently, with no diagnostic at all
(`src/fNBLI/fNBLI.cpp:111`):

```c
if (is_large && !supportAVX2())     // CPU do not support AVX2, but image need to be
    return NULL;                    // decompressed by AVX2
```

So on a Phenom II every large image ever produced by a modern machine is undecodable.

`src/fnbli_scalar.h` re-implements the whole 16-lane pipeline in portable integer C++:

| AVX2 primitive | scalar emulation |
|---|---|
| gather / scatter of the 16 contexts | read all `array_ctx[adr[k]]` first, then write all back |
| `_mm256_alignr_epi8` based `ring_shift_right` | explicit lane rotation |
| `align_by_mask_eq0` (renormalization) | renormalization words are consumed in ascending lane order |
| `clearAtStartOfLine` | U **and** V are cleared, not just Y |
| out-of-image lanes | filled with `MID_UV` / `MID_Y` |

The result is bit-identical to the AVX2 path, so `is_large = 1` files now decode on hardware that
predates AVX by a decade.

## 5. Robustness

A fuzzer (`tests/fuzz.py`) flips random bits in a stream and runs the decoder. The *original*
binaries segfault on a good share of those; `mtnbli` never does. What makes that possible:

* **read limits** — every word taken from the stream is bounds-checked
  (`nbliRansRead()` in `src/nbli/rANS.h`, `FnbliRANSd::rd()` in `src/fnbli_scalar.h`).
  A damaged stream sets a flag and is rejected instead of running off the buffer.
* **bounds-checked histograms** — a corrupted histogram code can make the run-length decoder
  overrun its table. Both `Histogram<>::decode` and `FnbliHist::decode` clamp every write.
  The *legal* spill of the last code word (which in the upstream code silently writes into the
  neighbouring member) is reproduced explicitly with `FNBLI_HIST_PAD` / `N_CTX + 1`.
* **clamped prefix sums** — an unnormalized histogram would otherwise make the decode lookup
  table be written out of bounds.
* **guard page + signal handler** — the input is `mmap`ed with a `PROT_NONE` page behind it, and
  each decode runs inside a `FAULT_TRY / FAULT_CATCH / FAULT_END` block
  (`src/safedecode.h`). Anything that still slips through is reported as
  `damaged stream (illegal memory access)` rather than killing the process.

Current status: **1200 fuzz runs (200 per format, 6 formats) — 0 crashes**.

## 6. Tests

```bash
make test                      # or:  ./tests/selftest.sh
```

The suite generates its own images and runs 303 checks:

1. fNBLI round-trips over 13 images (1×1, 2×3, 17×1, 1×17, 63×63, 128×128, 200×150, 300×200,
   512×512, noisy, flat, …) — bit-exact.
2. the same, tiled (`.tnbli`).
3. NBLI round-trips in every mode: rANS / Golomb × plain / AVP, RGB and gray.
4. the same, tiled, plus near-lossless at every distortion level (tolerance `2·near+1`).
5. **cross checks against the upstream binaries in both directions** — upstream encoder →
   `mtnbli` decoder, and `mtnbli` encoder → upstream decoder — plus a check that the individual
   tiles of a `.tnbli` decode standalone with the *upstream* tool.
6. negative tests: damaged streams must be rejected and must not leave an output file behind.

`tests/corpus_check.py` is the corresponding check on real files: it decodes every stream in a
directory with `mtnbli` *and* with the upstream tool and compares the two images bit by bit.

```
$ python3 tests/corpus_check.py /workspace/corpus mtnbli ../NBLI/fNBLI ../NBLI/NBLI
  ...
  21 passed, 0 failed, 0 skipped
```

## 7. Performance

`tests/bench.sh <image> [tiles] [thread list]` prints a scaling table.

**6000×4000 RGB (72 MB), 16 tiles** — Intel Xeon Platinum 8576C, 32 hardware threads:

| threads | compress | speed-up | decompress | speed-up | size |
|---:|---:|---:|---:|---:|---:|
| 1  | 2.023 s | 1.00× | 1.665 s | 1.00× | 11 758 142 |
| 2  | 1.032 s | 1.96× | 0.854 s | 1.95× | 11 758 142 |
| 3  | 0.785 s | 2.58× | 0.631 s | 2.64× | 11 758 142 |
| 4  | 0.559 s | 3.62× | 0.438 s | 3.80× | 11 758 142 |
| 6  | 0.530 s | 3.82× | 0.447 s | 3.72× | 11 758 142 |
| 8  | 0.540 s | 3.75× | 0.409 s | 4.07× | 11 758 142 |
| 16 | 0.621 s | 3.26× | 0.436 s | 3.82× | 11 758 142 |

**2000×2000 RGB (12 MB), 16 tiles** — the working set now fits, and the picture changes:

| threads | compress | speed-up | decompress | speed-up |
|---:|---:|---:|---:|---:|
| 1  | 0.357 s | 1.00× | 0.298 s | 1.00× |
| 2  | 0.186 s | 1.92× | 0.161 s | 1.85× |
| 4  | 0.098 s | 3.64× | 0.079 s | 3.77× |
| 8  | 0.061 s | 5.85× | 0.072 s | 4.14× |
| 16 | 0.072 s | 4.96× | 0.039 s | 7.64× |

The plateau on the 72 MB image is **memory bandwidth**, not the codec (a 72 MB image is written
and read once per pass); with a cache-resident image the decoder still reaches 7.6× at 16 threads.
On a 6-core Phenom II the codec sits squarely in the 3.5–4× region — which is the whole point.

The produced image is **bit-identical regardless of the thread count** (verified for
`-t 1 / 3 / 6 / 32`).

## 8. Source layout

```
src/
  mtnbli.cpp          CLI driver, job dispatch, PNM/PNG I/O
  fnbli_scalar.h      fNBLI in portable C++ : sampler, model, rANS, 16-lane wave-front emulation
  fnbli_api.h         compress / decompress entry points
  tiles.h             .tnbli container
  ThreadPool.h        ParallelFor (dynamic scheduling, default = all hardware threads)
  safedecode.h        guard page + SIGSEGV/SIGBUS recovery around each decode
  FileIO.h, CRC32.h
  nbli/               the upstream NBLI codec (hardened: bounded reads, clamped prefix sums)
  imageio/            PNM reader/writer + uPNG
tests/
  selftest.sh         303 self-contained checks incl. upstream cross checks
  corpus_check.py     decode a directory with mtnbli and with upstream, compare
  fuzz.py             robustness fuzzer
  bench.sh            thread scaling measurement
  isa_check.sh        verify the binary contains no forbidden instruction
  imglib.py imgcmp.py genimg.py tnbli_split.py tiles_check.py
```

Only two upstream files were modified, both for robustness only: `src/nbli/rANS.h` (bounded
reads, bounds-checked histogram, clamped prefix sums). Everything else in `src/nbli/` is the
original code.

## 9. License

GPLv3, like the original — see [LICENSE](LICENSE).

Copyright of the original NBLI / fNBLI codec: WangXuan95 (<https://github.com/WangXuan95/NBLI>).
