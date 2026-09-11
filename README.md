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

**Prebuilt binaries** ([v0.4-mt5](https://github.com/MixxxGit/mtNBLI/releases/tag/v0.4-mt5),
built by GitHub Actions, both with the portable ISA):

```
mtnbli-linux-x86_64     436 984 bytes   - any x86-64 Linux (glibc 2.35+)
mtnbli-win64.exe      1 278 589 bytes   - Windows 10 / 11 (and XP x64), statically linked
SHA256SUMS.txt
```

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
that none of them crashes. Current result: **33/33 identical, 0 crashes**.

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
                 <out> from -M               (.fnbli / .nbli / .tnbli)
To decompress:   <in>  .fnbli .nbli .tnbli
                 <out> .png, or .pgm .ppm .pnm with --pnm

  -v          verbose
  -f          force overwrite of an existing output file
  -x          store a CRC32 in the stream when compressing (recommended)
  -d          decode.  The inputs are our formats, the output is PNG (overrides --pnm)
  -M <N|F|MT> output format when compressing (default F):
                 N   NBLI   -> .nbli    slow, smallest
                 F   fNBLI  -> .fnbli   fast
                 MT  tiled  -> .tnbli   K independent strips, one per core
  -t <N>      number of threads            (default: all hardware threads)
  -T <N>      number of tiles for -M MT    (default 0 = auto, 2 tiles per thread)
  -pc <0..9>  PNG compression level        (default 6, 0 = stored)
  -g          NBLI: Golomb code tree instead of rANS   (slower, slightly smaller)
  -a          NBLI: advanced predictor                 (much slower, smaller)
  -0 .. -7    NBLI: distortion level, 0 = lossless (default), 1..7 = near-lossless
  --pnm       when decompressing, write PNM instead of PNG
```

`-M` chooses the format *and* the codec that goes with it. `MT` is the one you want for a single
big image: the file is split into horizontal strips that are compressed independently, so all
cores work on one picture. The strips themselves are fNBLI, unless you add `-g` / `-a` /
`-0..-7`, which are NBLI-only options and switch them to NBLI (`-M TN` says the same thing
explicitly). `-M F` together with one of those options is rejected instead of silently producing
a `.fnbli` that is not fNBLI.

`-d` is never required — an input is recognised by its content — but it makes the intent explicit
and it makes the output a PNG even when `--pnm` is set.

Examples

```bash
mtnbli -x -M MT photo.png  -o photo.tnbli   # tiled fNBLI, 2 tiles per thread, CRC32
mtnbli -f -d photo.tnbli                    # decode it with every core into photo.png
mtnbli -f -d -pc 9 photo.tnbli              # ... and squeeze the PNG as hard as possible
mtnbli -M N -a -x big.ppm                   # NBLI + advanced predictor, single stream
mtnbli -f -t 6 *.fnbli *.nbli               # decode a batch, 6 threads
```

### 2.0 `-pc` and the PNG writer

The PNG that comes out when decompressing is written by mtnbli itself, with its own
deflate implementation (see §2.3), and `-pc` is the same knob as the level of `zlib` / `libpng`:

| `-pc` | what it does | 3840×2160 RGB, 24.9 MB |
|-------|--------------|------------------------|
| 0 | stored, no compression at all (the behaviour of the older releases) | 24 900 548 B, 0.9 s |
| 1 | greedy matching, no lazy matching | 3 902 270 B, 1.1 s |
| 3 | greedy matching, deeper hash chain | 2 870 893 B, 1.3 s |
| 6 | **default** — lazy matching + adaptive row filters | 2 503 852 B, 1.9 s |
| 9 | deepest chain, all five row filters tried on every pixel | 2 338 089 B, 5.2 s |

The row filters matter as much as the deflate level: from `-pc 1` upwards a filter is picked per
scanline (Up at first, then Sub / Paeth, then all five), scored on a sample of the row so that an
8K frame does not have to be walked five times over.

### 2.1 Wildcards

`<in>` may contain `*` and `?`:

```bat
C:\> mtnbli.exe -v "C:\sources\yt-rnd-8K\2\*.png"          :: 115 images, all cores
C:\> mtnbli.exe -v "D:\shots\*\frame_?.png"                :: wildcard in the middle too
```

`cmd.exe` does **not** expand `*.png` itself (that is a Unix shell habit), so `mtnbli.exe` does it
— on Windows with `FindFirstFileW`, on Linux with `glob()`. A quoted pattern works on both
systems, and a pattern that matches nothing is reported as one failed input with the name you
typed, not as a silent success. Only input names are expanded, never a `-o` destination; each
expanded file gets its own output name.

Non-ASCII directories work too: the arguments are taken from the Unicode command line
(`CommandLineToArgvW`) instead of the ANSI `argv` the C runtime normally builds, and files are
opened with `_wfopen` after a UTF-8 → UTF-16 conversion.

### 2.2 What `-v` prints (and why it no longer looks like a hang)

Everything used to be printed after the last file was finished — with 115 images that is a minute
of silence and then a wall of text. Now:

* **every result is printed the moment its file is done** (so the list arrives in completion
  order, not in argument order);
* a **status line** in front of it says what the workers are doing *right now*:

```
[00:12] 47/115 (40%)  eta 00:18  tiles 892/2760  48 213 kB/s  | now: yt_Rv5oymOuZCc_00-19-41.png, yt_N1VSDHZ7SYo_00-12-07.png +10 more
```

  On a console it is redrawn in place with `\r`; when stdout is redirected to a file it is
  written as a normal line at most every 5 s, so a log does not fill up with it. `tiles` is
  the progress inside the `.tnbli` tiles — with `-T 0` one 8K frame is dozens of tiles, so
  "files done" alone would sit still for a minute.
* a failure is **always** reported, even without `-v`.

At the end there is an overview of what the run actually achieved:

```
summary: 115 compressed, 0 decompressed, 0 failed, 38.5787 s wall (12 threads)
----------------------------------------------------------------------------------
compressed   : 114 files, 3.63 GB -> 2.55 GB   (-29.7 %, saved 1.08 GB)
               average 3.0214 BPP   best 0.8874 (yt_N1VSDHZ7SYo_00-02-23.png)   worst 7.1941 (yt_267a6c30330_00-30-35.png)
               48.0 MB/s, 13.5x on 12 threads (521.3 s of work in 38.58 s)
failed       : 1
               yt_267a6c30330_00-00-47.png : output exists
```

`13.5x` is the sum of the per-file times divided by the wall clock time, i.e. how much work the
threads really got through in parallel — it is the number to look at when you change `-t`.
Without `-v` nothing is printed per file (except failures); the summary appears whenever more
than one file was given.

The exit status is the number of files that failed.

### 2.3 Why the PNG writer has its own deflate

`src/imageio/deflate.c` is a complete, self-contained RFC 1951 encoder with a zlib wrapper —
LZ77 with a 64 KB sliding window, length-limited canonical Huffman codes and dynamic block
headers. It is there because linking `zlib` would break the one promise this project makes:

* a distribution `libz` is compiled for a **much newer** CPU baseline than an AMD Phenom II, and
  the mingw copy that comes with the cross toolchain is built with BMI enabled — `isa_check.sh`
  finds `TZCNT` in it, and `TZCNT` on a Phenom II decodes as `BSF` with the operand order
  reversed (wrong result, no trap);
* the codec should stay a handful of files with no external dependency.

Two details are worth knowing:

* the encoder **streams**. It keeps only a 64 KB window, so an 8K RGB frame (≈ 100 MB) does not
  need a second 100 MB buffer — the old stored-only writer did;
* the deflate output goes through a sink that writes one IDAT chunk per 64 KB, which is why a
  `.png` written by mtnbli can contain several IDAT chunks. That is standard, and the upstream
  `NBLI`/`fNBLI` tools (they read PNG with uPNG) open those files without complaint.

Every level 0..9 is checked against Python's `zlib.decompress` in `tests/defl_check.py` (§8).

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

Up to about 16 tiles the cost is negligible. `-T 0` (auto) picks `2 × n_threads`, where
`n_threads` is exactly the number of threads the run is going to use — `-t N` if you gave it,
otherwise every hardware thread (minus the cgroup quota) — clamped so that no strip is shorter
than 96 rows.

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

The suite generates its own images and runs 347 checks:

1. fNBLI round-trips over 13 images (1×1, 2×3, 17×1, 1×17, 63×63, 128×128, 200×150, 300×200,
   512×512, noisy, flat, …) — bit-exact.
2. the same, tiled (`.tnbli`).
3. NBLI round-trips in every mode: rANS / Golomb × plain / AVP, RGB and gray.
4. the same, tiled, plus near-lossless at every distortion level (tolerance `2·near+1`).
5. **cross checks against the upstream binaries in both directions** — upstream encoder →
   `mtnbli` decoder, and `mtnbli` encoder → upstream decoder — plus a check that the individual
   tiles of a `.tnbli` decode standalone with the *upstream* tool.
6. negative tests: damaged streams must be rejected and must not leave an output file behind.
7. wildcards: `dir/*.ppm`, `dir/*/*.ppm`, `a?.ppm`, a pattern that matches nothing, and one
   output name per expanded file.
8. the live progress of `-v` and the summary block.
9. `-M N|F|MT`: the right suffix, lossless round-trips, the tile count, and that the removed
   `-N` and the impossible combinations (`-M F -a`, `-M Q`) are rejected.
10. `-d` and `-pc 0..9`: every level must decode back to the original pixels, the size must
   never grow with the level, and the default must be 6.
11. the deflate itself: 8 buffers × 10 levels, each inflated again by Python's `zlib`.
12. `bench_compare.py` itself: it runs, every round trip it makes comes back bit identical, it
    numbers its stages, it really measured the CPU time of the child processes (and `--diag` says
    where it got it from), it writes its csv/json, it deletes its work directory, it deletes the
    reference files it no longer needs and it leaves the image folder alone.

The cross checks of item 5 need the *unmodified upstream* `NBLI` and `fNBLI` binaries. Point
`REFDIR` at them (`REFDIR=/path/to/NBLI ./tests/selftest.sh`, default `../NBLI`) or build them
with `bash tests/build_ref.sh`; without them those checks are reported as skipped.

`tests/corpus_check.py` is the corresponding check on real files: it decodes every stream in a
directory with `mtnbli` *and* with the upstream tool and compares the two images bit by bit.

```
$ python3 tests/corpus_check.py /workspace/corpus mtnbli ../NBLI/fNBLI ../NBLI/NBLI
  ...
  22 passed, 0 failed, 0 skipped
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

### 7.4 Comparing with the author's codecs (`tests/bench_compare.py`)

`tests/bench.sh` measures `mtnbli` against itself. `bench_compare.py` measures it against the
**upstream** NBLI / fNBLI: you point it at a folder of binaries and a folder of images, it pushes
every image through every codec and back to PNG, and it writes a report that ends with an
analytical summary which the script computes itself.

```bash
python3 tests/bench_compare.py -c "C:\progz\NBLI" -i "C:\sources\yt-rnd-8K"
python3 tests/bench_compare.py -c ../NBLI -i ./shots -o ./report --keep --t1
make bench-compare BENCH_ARGS="-c ../NBLI -i ./shots"      # the same thing through make
```

`-c` only has to contain the three binaries (`fNBLI`, `NBLI`, `mtnbli`, with or without `.exe`);
on Linux an `.exe` is started through wine, so the Windows binaries can be compared here as well.
Five codecs are measured, in two modes:

| codec | what it is | measured in |
|---|---|---|
| `fNBLI (upstream)` | the author's `fNBLI`, single threaded | BATCH and SINGLE |
| `NBLI (upstream)` | the author's `NBLI`, single threaded | BATCH and SINGLE |
| `fNBLI (mtnbli -M F)` | ours, same stream as upstream | BATCH and SINGLE |
| `NBLI (mtnbli -M N)` | ours, same stream as upstream | BATCH and SINGLE |
| `mtNBLI (mtnbli -M MT)` | ours, tiled — one strip per core | BATCH and SINGLE |

* **BATCH** — the whole folder in one command line, which is how you would actually use it.
* **SINGLE** — one command per file, times summed over the files, so the cost of one process
  start per file shows up. Comparing the two tells you what parallelism buys without having to
  know which codec is parallel.

Two things are measured for every codec and every phase: **wall clock** and **CPU time**. CPU
time is the cost of the job in core-seconds, so it is the column that compares one core of ours
with one core of theirs — the right number when the author's `fNBLI` is running its AVX2 path
and ours is not. The script detects AVX2 (and the cgroup quota) and says so in the report.

Getting that CPU time is less obvious than it looks, and the script does it portably:

| platform | where `cpu s` comes from | where `peak_rss_mb` comes from |
|---|---|---|
| Linux, macOS | `os.times()` (`children_user + children_system`) — accounts for every child, including a wine tree | `/proc/<pid>/status` `VmHWM`, polled for the whole tree |
| Windows | `GetProcessTimes()` on a **handle the script owns itself** (subprocess closes its handle the moment it reaps the child, and `OpenProcess` on a dead pid fails; `os.times()` has no `children_*` on Windows and `resource` does not exist there) | `GetProcessMemoryInfo()` `PeakWorkingSetSize` |
| any | `psutil`, sampled while the codec runs — used whenever the above gives nothing | same |

`--diag` prints which of those it used on your machine, before the run starts. If none of them
works, `cpu s` / `cores` stay empty and the report says so instead of printing a zero.

The run is also **loud**: every step is announced as `[stage 7/45] …` with a progress bar and an
elapsed time inside it, so a run over a folder of 8K frames never looks like it hung.

```
   codec                        encode                            decode                        stream                 bit exact
                             sec     MB/s    cpu s  cores       sec     MB/s    cpu s  cores        bytes    %raw      bpp
   --------------------------------------------------------------------------------------------------------------------------------
   fNBLI  (upstream)             0.185     39.2    0.200    1.1      0.159     45.7    0.160    1.0    3 689 664   50.8%  11.051       7/7
   NBLI   (upstream)             0.569     12.8    0.570    1.0      0.514     14.1    0.510    1.0    3 640 714   50.1%  10.905       7/7
   fNBLI  (mtnbli -M F)          0.183     39.7    0.310    1.7      0.147     49.3    0.290    2.0    3 689 664   50.8%  11.051       7/7
   NBLI   (mtnbli -M N)          0.323     22.5    0.610    1.9      0.258     28.2    0.510    2.0    3 640 714   50.1%  10.905       7/7
   mtNBLI (mtnbli -M MT)         0.143     50.8    0.350    2.4      0.114     63.9    0.330    2.9    3 701 476   51.0%  11.087       7/7
```

and the part that is computed, not typed in:

```
 comparison                       encode decode round trip   size cores used
 ---------------------------------------------------------------------------
 fNBLI  : ours vs upstream        1.01x   1.08x 1.04x      +0.00% 1.8
 NBLI   : ours vs upstream        1.76x   1.99x 1.86x      +0.00% 1.9
 mtNBLI : tiled vs upstream fNBLI 1.30x   1.40x 1.34x      +0.32% 2.7

   the same comparison in core-seconds, i.e. one core of ours against one core of theirs
 comparison (CPU time)            encode decode round trip
 ---------------------------------------------------------
 fNBLI  : ours vs upstream        0.65x   0.55x 0.60x
 NBLI   : ours vs upstream        0.93x   1.00x 0.96x
 mtNBLI : tiled vs upstream fNBLI 0.57x   0.48x 0.53x
```

That last block is the honest one to read on a machine with AVX2: **per core** the author's
`fNBLI` is roughly 1.7× faster (it uses AVX2, we never do), and `mtnbli` wins it back with
threads — 1.34× on the round trip in this run, more on an unthrottled 6- or 8-core machine, and
much more on a folder of 8K frames where the tiled container can use every core.

Correctness is checked on the way, but not by comparing PNG bytes — that would only compare our
PNG writer with the author's. Every stream is decoded once more into a **raw PNM** (untimed) and
compared against the reference the same route produces from the source image. The comparison is a
**128 bit BLAKE2b hash** (`hashlib.blake2b(digest_size=16)`, C speed, nothing to install) plus the
byte count, so the decoded images never have to be kept: each one is hashed and deleted on the
spot. The report also cross decodes: the author's streams read by `mtnbli`, and ours read by the
author's tool. `--strict` adds a pure-Python pixel comparison of the decoded PNGs (slow on big
images).

Disk is treated the same way everywhere — **every intermediate is deleted the moment it has been
consumed**:

* the reference is built in chunks (`--ref-chunk-mb`, default 512 MB of raw pixels), and each
  chunk's `.fnbli` goes away as soon as its `.pnm` exists, and the `.pnm` as soon as it is hashed;
* the decoded PNG of a codec is deleted right after that codec's decode has been measured (it is
  never compared byte-wise — the check re-decodes the stream itself);
* each raw copy is deleted right after it has been hashed;
* a codec's streams are deleted after its check, except the one or two files the cross decode at
  the end still reads.

Everything the run produces goes into one work directory which is **deleted at the end**; the
originals are only ever read. What stays is `-o` (default `./bench_report`): `report.txt`,
`results.csv` with every single measurement (wall, CPU, peak RSS, bytes, exit code) and
`results.json`.

Useful switches: `-m batch|single|both`, `-r/--runs` (repetitions, best kept), `-t/-T`
(threads / tiles for mtnbli), `--t1` (add a single-thread `-M MT -t 1` baseline), `--nbli-opts "-g"`,
`--pc` (PNG level for the decoder, default 0 = uncompressed like the author's tools),
`--limit`, `--filter "*.png"`, `--keep`.

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
  imageio/            PNM reader/writer, PNG writer, and its own deflate
    deflate.c/.h        RFC 1950/1951 encoder, levels 0..9, streams out through a sink
tests/
  selftest.sh         347 self-contained checks incl. upstream cross checks
  corpus_check.py     decode a directory with mtnbli and with upstream, compare
  fuzz.py             robustness fuzzer
  bench.sh            thread scaling measurement
  isa_check.sh        verify the binary contains no forbidden instruction (ELF and PE)
  win_check.sh        run mtnbli.exe under wine, compare with the native build
  defl_test.c         pushes 8 pathological buffers through deflate at levels 0..9
  defl_check.py       inflates every result with Python zlib and compares it back
  bench.sh            thread scaling of mtnbli
  bench_compare.py    mtnbli vs the upstream NBLI / fNBLI on a folder of images
  imglib.py imgcmp.py genimg.py tnbli_split.py tiles_check.py
```

Only two upstream files were modified, both for robustness only: `src/nbli/rANS.h` (bounded
reads, bounds-checked histogram, clamped prefix sums). Everything else in `src/nbli/` is the
original code.

## 9. License

GPLv3, like the original — see [LICENSE](LICENSE).

Copyright of the original NBLI / fNBLI codec: WangXuan95 (<https://github.com/WangXuan95/NBLI>).
