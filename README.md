# mtnbli — multi-threaded, SIMD-free NBLI / fNBLI codec

[![build](https://github.com/MixxxGit/mtNBLI/actions/workflows/build.yml/badge.svg)](https://github.com/MixxxGit/mtNBLI/actions/workflows/build.yml)

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

### 1.1 Windows (`mtnbli.exe` for Windows 10 / 11, and for that matter Windows XP x64)

Cross-compiled from Linux with mingw-w64:

```bash
sudo apt-get install mingw-w64       # or your distro's equivalent
make win64                           # -> mtnbli.exe   (static, ~1.2 MB, no extra DLLs)
make win64-isa                       # same ISA proof, for the PE file
make win64-check                     # run the .exe under wine, compare with the native build
```

`mtnbli.exe` is statically linked against the mingw runtime, so it needs nothing but
`KERNEL32.dll` / `ADVAPI32.dll` / `msvcrt.dll` — all present on every Windows. It carries the
same `-mno-*` flags as the Linux build and is therefore equally happy on a Phenom II.

The POSIX-only pieces have a Windows counterpart:

| | Linux | Windows |
|---|---|---|
| catch a wild read | `sigaction(SIGSEGV)` + `siglongjmp` | vectored exception handler + `longjmp` |
| guard page behind the input | `mmap` + `mprotect(PROT_NONE)` | `VirtualAlloc(MEM_RESERVE)`, page left uncommitted |
| CPU name in the banner | `/proc/cpuinfo` | `CentralProcessor\0\ProcessorNameString` registry key |
| non-ASCII file names | (bytes are bytes) | UTF‑8 → UTF‑16, `CreateFileW` |

`make win64-check` (needs `wine`) decodes every stream of a directory with both builds and
compares the two outputs byte for byte, then feeds 40 damaged streams to the `.exe` and asserts
that none of them crashes. Current result: **23/23 identical, 0 crashes**.

### 1.2 Continuous integration (or: how to get the `.exe` without installing mingw)

Every push and pull request runs [`.github/workflows/build.yml`](.github/workflows/build.yml):

| job | what it does |
|---|---|
| `linux` | portable-ISA build → full self test → ISA proof → 120 fuzz runs |
| `windows` | `make win64` → ISA proof of the PE file → the `.exe` under wine |
| `release` | on a `v*` tag: attaches `mtnbli-linux-x86_64`, `mtnbli-win64.exe` and a `SHA256SUMS.txt` to the release |

Both binaries are downloadable from the **Artifacts** of a completed run, and, for a tagged
commit, from the [releases page](https://github.com/MixxxGit/mtNBLI/releases).

The reference binaries used by the cross checks are built by

```bash
bash tests/build_ref.sh [destdir]      # clones WangXuan95/NBLI and builds NBLI + fNBLI
```

Upstream `fNBLI` is AVX2-only, so this needs an AVX2 machine; if it fails the cross checks are
simply skipped (that is why the step is best effort in CI).
Note that upstream's `uPNG.c` says `#include "upng.h"` while the file is called `uPNG.h`, so the
script drops a lowercase symlink next to it before compiling.

A prebuilt `mtnbli.exe` is attached to every release.

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

Two things to keep in mind:

* the tile count is **baked in at compression time** — a `.tnbli` with 8 tiles can never use more
  than 8 cores, however big the machine that later decodes it. If you compress on a small box and
  decode on a big one, pass `-T` explicitly.
* `-t` defaults to `min(CPUs you have, CPUs your cgroup lets you burn)`, reading the CFS quota
  from `/sys/fs/cgroup/cpu.max`. Oversubscribing a throttled container is measurably slower, not
  just useless (see §7.3).

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

The cross checks of item 5 need the *unmodified upstream* `NBLI` and `fNBLI` binaries. Point
`REFDIR` at them (`REFDIR=/path/to/NBLI ./tests/selftest.sh`, default `../NBLI`) or build them
with `bash tests/build_ref.sh`; without them those checks are reported as skipped.

`tests/corpus_check.py` is the corresponding check on real files: it decodes every stream in a
directory with `mtnbli` *and* with the upstream tool and compares the two images bit by bit.

```
$ python3 tests/corpus_check.py /workspace/corpus mtnbli ../NBLI/fNBLI ../NBLI/NBLI
  ...
  21 passed, 0 failed, 0 skipped
```

## 7. Performance — what parallelises and what does not

This is the part people get wrong, so it comes with numbers. `mtnbli` **decodes both formats
bit-exactly**, but the two formats are *not* equally parallelisable:

| workload | fNBLI | NBLI | parallel? |
|---|---|---|---|
| **one** legacy `.fnbli` / `.nbli` file | yes | yes | **no — 1 core, always** |
| **many** files in one invocation | yes | yes | **yes** |
| **one** `.tnbli` file (tiled) | yes | yes | **yes** |

A legacy stream is one adaptive rANS/Golomb stream: symbol *N* needs the complete coder state
left behind by symbols *0..N-1*. That is a serial dependency, not an implementation detail — so
**no decoder, however clever, can make a single legacy file use two cores.** Measured, one
800×600 file, best of 3:

| threads | 1 | 8 | 32 |
|---|---:|---:|---:|
| one `.fnbli` | 0.046 s | 0.045 s | 0.046 s |
| one `.nbli`  | 0.092 s | 0.092 s | 0.093 s |

Flat. Threads change nothing. Everything below is about the two cases that *do* scale.

### 7.1 Many files at once (both formats, no re-encoding needed)

16 legacy 800×600 streams in a single invocation, best of 3:

| | `-t 1` | `-t 8` | speed-up |
|---|---:|---:|---:|
| 16 × `.fnbli` | 0.805 s | 0.160 s | **5.0×** |
| 16 × `.nbli`  | 1.500 s | 0.370 s | **4.1×** |

This is the mode to use for an existing archive: `mtnbli -f *.fnbli *.nbli` and all cores are busy.

### 7.2 One big file — needs the `.tnbli` container

`tests/bench.sh <image> [tiles] [thread list]` prints the table. 3000×2000 RGB (18 MB), 12 tiles:

| threads | compress | speed-up | decompress | speed-up | size |
|---:|---:|---:|---:|---:|---:|
| 1  | 0.522 s | 1.00× | 0.430 s | 1.00× | 3 423 332 |
| 2  | 0.270 s | 1.93× | 0.222 s | 1.94× | 3 423 332 |
| 3  | 0.195 s | 2.68× | 0.150 s | 2.87× | 3 423 332 |
| 4  | 0.151 s | 3.46× | 0.115 s | 3.74× | 3 423 332 |
| 6  | 0.133 s | 3.92× | 0.101 s | 4.26× | 3 423 332 |
| 8  | 0.113 s | 4.62× | 0.103 s | 4.17× | 3 423 332 |
| 16 | 0.068 s | 7.68× | 0.106 s | 4.06× | 3 423 332 |

The produced image is **bit-identical regardless of the thread count** (verified for
`-t 1 / 3 / 6 / 32`).

### 7.3 Caveat about these numbers

The machine that produced them reports 32 CPUs but runs under a CFS quota of **4.00 CPU**
(`/sys/fs/cgroup/cpu.max = 400000 100000`). Speed-ups therefore saturate around 4–5×, and pushing
far past the quota is *counter-productive*: in the 16-file batch, `-t 8` took 0.341 s but
`-t 16` took 0.439 s and `-t 32` 0.459 s, because the cgroup gets throttled and pays for extra
context switches. `tests/bench.sh` detects the quota and says so in its header — do not read a
table as "the codec stops scaling" without checking that line first.

On real, unthrottled hardware expect the scaling to continue to the physical core count. On a
6-core Phenom II X6 that means roughly 5–6× on a `.tnbli` and on batches.

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
  isa_check.sh        verify the binary contains no forbidden instruction (ELF and PE)
  win_check.sh        run mtnbli.exe under wine, compare with the native build
  imglib.py imgcmp.py genimg.py tnbli_split.py tiles_check.py
```

Only two upstream files were modified, both for robustness only: `src/nbli/rANS.h` (bounded
reads, bounds-checked histogram, clamped prefix sums). Everything else in `src/nbli/` is the
original code.

## 9. License

GPLv3, like the original — see [LICENSE](LICENSE).

Copyright of the original NBLI / fNBLI codec: WangXuan95 (<https://github.com/WangXuan95/NBLI>).
