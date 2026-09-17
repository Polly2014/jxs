# [JPEG XS Reference Software - 2nd Edition](https://jpeg.org/jpegxs/software.html)

## License and copyright

Please see the accompanied LICENSE.md for more details.

## 中文总结：RK3588 并行优化评估

本分支增加帧内 OpenMP 并行和编码器原生帧级 `-j N` 并行，保留默认串行构建，并提供真实素材 E2E、性能对比及 RK3588 复测脚本。**已完成的是 Windows / Linux x86-64 主机验证，尚无 RK3588 板端实测，实际 SDK 交叉编译也未验证。**

仓库保留原有源码结构，不把压缩包的交付目录布局强行套用到工程中：

| 内容 | 位置 |
| --- | --- |
| 编解码源码、命令行程序 | `libjxs/`、`programs/` |
| RK3588 交叉编译配置 | `cmake/rk3588-linux.cmake` |
| 离线回归、素材下载、E2E、板端复测 | `extras/` |
| 完整 99 组性能结果 | [reports/host-final/results.json](reports/host-final/results.json) |
| 全量性能汇总 | [reports/host-final/summary.txt](reports/host-final/summary.txt) |
| 测试输入来源和哈希清单 | [reports/host-final/input-manifest.json](reports/host-final/input-manifest.json) |

报告中的个人本地绝对路径已替换为相对构建目录；逐次耗时、质量指标、码流哈希等原始测量记录未改动。本仓库不提交本地构建产物、原始视频、大体积素材或交付 ZIP，可运行 `extras/prepare_media.py` 根据来源和 SHA-256 准备测试输入。

主机严格无损编码对比：i9-10900X、GCC 13.1.0、Release、1 次预热、3 次测量，8 个请求线程或帧 worker。

| 样本 | 原版编码 | 并行编码 | 编码加速 | 编码＋解码加速 |
| --- | ---: | ---: | ---: | ---: |
| 仙女座天文图 1920×1080 | 1.4376 秒 | 0.7069 秒 | 2.03× | 1.65× |
| 仙女座天文图 3840×2160 | 5.0665 秒 | 2.2507 秒 | 2.25× | 1.85× |
| Big Buck Bunny 连续 12 帧，320×180 | 0.6136 秒 | 0.2156 秒 | 2.85× | 1.13× |

这些是包含进程启动、文件 I/O 和上下文初始化的主机耗时，不是 RK3588 性能，也不是纯算法内核耗时。编码加速不能当作全链路加速；小图可能变慢。固定码率不等于数学无损，原版的全范围 16-bit 问题也未纳入本次支持保证。历史测试 ID `mars_1920` / `mars_3840` 实际对应仙女座星系，保留旧名仅用于维持报告引用关系。

离线回归覆盖 51 次回环，包括 8/10/12-bit、YUV 420/422、奇数尺寸、帧 worker 及错误处理。真实素材对比的码流和解码输出与原版一致，33 组无损结果精确重建。以上不是 ISO 符合性认证；参考 Part 4 数据集未提供。

本分支基于上游 `b5aa15dbd551d8c110878515b26d5a42908b5815`。相对本仓库原来的 `94453b6`，也包含上游 `7764e8d` 的码流 profile 字段写入修复，避免遗漏本次测试所依赖的基线行为。

构建、测试命令和许可证说明见下文。原软件许可证允许评估和测试；产品集成需另行确认授权，且不授予专利许可。

## Parallel CPU evaluation (RK3588 target)

This branch adds optional OpenMP CPU parallelism. The target is RK3588 Linux
(four Cortex-A76 and four Cortex-A55 cores), but **there is no RK3588 board
measurement yet**. Windows x86-64 host timings are not estimates of board speed.
This is an evaluation implementation, not a production qualification.

The repository license permits evaluation/testing derivatives; product
integration needs separate licensing consideration, and no patent license is
granted. See `LICENSE.md`.

### Build and run

The default remains serial and has no OpenMP dependency:

```sh
cmake -S . -B build-serial -DCMAKE_BUILD_TYPE=Release -DJXS_ENABLE_OPENMP=OFF
cmake --build build-serial --parallel 4
cmake -S . -B build-parallel -DCMAKE_BUILD_TYPE=Release -DJXS_ENABLE_OPENMP=ON
cmake --build build-parallel --parallel 4
ctest --test-dir build-parallel --output-on-failure
```

Use `-G "MinGW Makefiles"` with MinGW on Windows. `python -m cmake` is also
supported when CMake is installed as a Python package. GCC/OpenMP is the tested
backend; a requested OpenMP build fails configuration rather than silently
disabling parallelism if its runtime is unavailable. Python 3 enables the
offline CTest regression; the codec itself does not require Python.

There are two distinct execution strategies:

| Strategy | Control | Parallel work | Trade-off |
| --- | --- | --- | --- |
| Within a frame | `OMP_NUM_THREADS` with ordinary encoder/decoder commands | Independent DWT rows/columns, linear NLT, reversible color transform; encoder precinct-column analysis and quantization | Preserves frame-at-a-time processing; serial entropy packing limits scaling |
| Across video frames | Encoder `-j N -f FIRST -n COUNT` | Independent native C workers with private images, configurations and codec contexts | Improves sequence throughput, uses memory for up to N active frames, does not reduce individual-frame latency |

Frame workers use dynamic scheduling and suppress nested library teams.
Output filenames preserve frame indices, regardless of completion order.
The frame decoder remains sequential; its transforms can use the first strategy.
The image transforms avoid starting teams for small work units.

For example, in a Linux shell:

```sh
export OMP_NUM_THREADS=4
export OMP_WAIT_POLICY=PASSIVE
export OMP_DYNAMIC=FALSE
export OMP_MAX_ACTIVE_LEVELS=1

# Existing profile/settings are preserved, not silently retiled.
build-parallel/bin/jxs_encoder \
  -c 'p=Main444.12;rate=6;bw=20;fq=8' input.ppm output.jxs

# Native frame parallelism; not a Python multiprocess wrapper.
build-parallel/bin/jxs_encoder -j 4 -f 1 -n 12 \
  -c 'p=Main444.12;rate=6;bw=20;fq=8' \
  'input_%04d.ppm' 'output_%04d.jxs'
```

In PowerShell set `$env:OMP_NUM_THREADS='4'` and
`$env:OMP_WAIT_POLICY='PASSIVE'`, and use Windows paths / `.exe` binaries.

`-j > 1` requires an OpenMP build, a positive explicit frame count, nonnegative
representable indices, and exactly one `%d` or `%0Nd` conversion (`N=1..10`)
in **both** paths. It rejects `-D`, ambiguous patterns, missing requested input
frames and invalid worker counts. A failed job produces a nonzero overall exit;
other completed output frames may remain. It reads codec-supported image/raw
frame sequences, not MP4 containers; demux/decode source video first.

For precinct-column parallelism, `cw=0` means one full-width column and cannot
parallelize rate allocation. The actual column width in this implementation is
`8 * cw * max_component_sx * 2^nlx`, not simply `8 * cw`. Profile constraints
still apply; the implementation does **not** change them to force parallelism.
Changing `cw`, transforms, profiles or bitrate changes the experiment:
compare the original and candidate with the **same** configuration.

### Lossless versus rate-controlled encoding

JPEG XS rate-controlled configurations are not automatically mathematically
lossless. Both modes are evaluated separately. A source-exact RGB example is:

```sh
build-parallel/bin/jxs_encoder \
  -c 'p=unrestricted;size=-1;bw=8;fq=0;cpih=rct;cw=0;nly=1' \
  input.ppm output.jxs
```

Offline regression checks exact source reconstruction for 8/10/12-bit samples,
including odd dimensions, YUV 4:2:0/4:2:2 and horizontal/vertical decomposition variants.
Real-media runs additionally check byte-identical original/candidate codestreams,
decoded image equality and cross-decoding with the untouched reference.
These checks establish regression equivalence, **not ISO conformance certification**.

Full-range 16-bit data is not qualified: the untouched baseline exhibited
non-exact and inconsistent decoding on that probe. Reproduce it with
`python extras/regression.py --bin ORIGINAL_BIN --reference ORIGINAL_BIN --probe-16bit`.
This pre-existing issue is not hidden by calling the result "lossless".
The ISO Part 4 reference corpus and `difftest_ng` required by the original
`extras/selftest_part4.sh` are not bundled and have not been supplied.

### Real data, E2E and performance reproduction

The original comparison commit is
`b5aa15dbd551d8c110878515b26d5a42908b5815`. Preserve an original Release build
before changing sources; do not rebuild that directory from modified sources.
Use an isolated checkout, for example:

```sh
git worktree add --detach build-original-src b5aa15dbd551d8c110878515b26d5a42908b5815
# Linux only: fix the original CLI's unavailable strcat_s, not the codec.
git -C build-original-src apply --ignore-space-change ../extras/baseline-linux.patch
cmake -S build-original-src -B build-original -DCMAKE_BUILD_TYPE=Release
cmake --build build-original --parallel 4
```

For Windows the untouched original builds with MinGW without that portability
patch. The included host reference binaries were built before any changes.
On Linux apply the patch using an **absolute path** if invoking `git -C` from
a different directory; `--ignore-space-change` handles the original CRLF files.
Only the bounded configuration-string append changes.
Use identical compiler, optimization flags and affinity for both builds.

```sh
python -m pip install -r extras/requirements-evaluation.txt
python extras/prepare_media.py --output test-data --frames 12

OMP_WAIT_POLICY=PASSIVE python extras/evaluate.py \
  --reference build-original/bin --candidate build-parallel/bin \
  --data test-data --output benchmark-results/run-001 \
  --threads 1 2 4 8 --warmups 1 --repeats 3
```

Use a **new/empty** output directory for every run. The runner fails on missing
outputs, content mismatches, unexpected return codes and timeouts; it does not
count partial sequences as successful.

The data manifest records URLs, attributions, license references, download SHA-256,
conversion operations and every prepared input hash:

| Input | Provenance and limits |
| --- | --- |
| Astronaut, 512x512 RGB | NASA public-domain photograph via scikit-image |
| Odd-size grayscale, 509x507 | Crop/gray conversion of that photograph |
| 12-bit grayscale | Expanded from the 8-bit photograph; **not native HDR capture** |
| Andromeda Galaxy, 1920x1080 and 3840x2160 | Crop/downsample of NASA/JPL/California Institute of Technology PIA04921 (GALEX), originally 6200x6200; astronomy imagery, not natural-camera footage |
| 12 consecutive video frames, 320x180 | Big Buck Bunny at 60 seconds; real video sequence, **animation rather than camera footage** |

Big Buck Bunny attribution: **(c) copyright 2008, Blender Foundation /
www.bigbuckbunny.org**, CC BY 3.0. License: https://peach.blender.org/about/.
Retain attribution when redistributing derived frames. Downloaded media and
generated measurements are ignored by Git.

The historical local filenames / benchmark IDs `mars.jpg`, `mars_1920` and
`mars_3840` identify **Andromeda**, not Mars. The verified NASA metadata is at
https://images-api.nasa.gov/search?nasa_id=PIA04921. Retaining those IDs preserves
the raw measurement references; it does not identify the depicted subject.

`results.json` contains raw repeated timings, median/p95, encoding/decoding
throughput, speedups, sampled memory/thread counts, correctness/quality metrics,
binary hashes and CMake build settings. `summary.txt` is a compact comparison.
Times are **CLI wall time**, including process startup, file I/O, context setup
and codec, after warmup with a warm filesystem cache. They are not pure kernel
times. Small inputs on a shared Windows host are noisy; three repeats are an
initial comparison, not a stable tail-latency study.

The `main` measurement label means full-width precincts with `nly=2` under the
explicit unrestricted configuration, **not** a claim about the Main profile.
`columns` uses explicit legal column settings, and `lossless` disables the rate
budget with reversible settings. Some small images still have only one column;
the report includes the actual column count.

Sequence FPS is batch throughput, not individual-frame latency. The original
decoder silently stops when a later lossless frame exceeds its first input
buffer. For fair variable-size sequence comparisons, **both** versions therefore
decode those frames with individual CLI invocations; `decoder_invocations`
records this scope. The candidate also fixes variable-size sequence buffers,
honors `-n`, fails on missing requested frames, and releases per-frame decoder
state instead of leaking it.

Small images / single-column cases can become slower with threads; parallelism
is opt-in for this reason. Do not summarize only the fastest case, and do not
extrapolate host acceleration to RK3588.

### Recorded host results

Intel Core i9-10900X (10 cores / 20 logical CPUs), Windows, MinGW GCC 13.1.0,
Release `-O3 -DNDEBUG`; candidate additionally uses `-fopenmp`.
One warmup, three randomized/interleaved measured repetitions, passive OpenMP
waiting. The full run contains 99 result rows and 25 negative CLI cases.
All compared codestreams and decoded outputs matched the original; all 33
lossless rows reconstructed source samples exactly.

Representative **lossless encoding** results (seconds, median):

| Input | Strategy | Original | Candidate | Encode speedup | Encode+decode speedup |
| --- | --- | ---: | ---: | ---: | ---: |
| Andromeda 1920x1080 | Intra-frame, 8 requested threads | 1.4376 | 0.7069 | 2.03x | 1.65x |
| Andromeda 3840x2160 | Intra-frame, 8 requested threads | 5.0665 | 2.2507 | 2.25x | 1.85x |
| BBB, 12 consecutive 320x180 frames | 8 frame workers | 0.6136 | 0.2156 | 2.85x | 1.13x |
| Astronaut grayscale 509x507 | Intra-frame, 8 requested threads | 0.1035 | 0.1252 | 0.83x | See full report |

The video times cover the entire 12-frame batch. Its roundtrip comparison
includes 12 separate decoder startups on both versions, as explained above.
Encoding speedup must not be reported as end-to-end roundtrip speedup.
Four-thread lossless encoding speedups were 1.82x (1080p), 2.17x (4K), and
2.31x (video frame workers). Four cores are not equivalent to RK3588's four A76s.
The full-width 4K rate-controlled case only improved about 1.06x at 8 threads;
the serial entropy stages remain a bottleneck.

The local report is `benchmark-results/host-final/results.json`, with
`summary.txt` alongside it. Source media, conversions and hashes are in
`test-data/manifest.json`. The delivery archive includes these reports and the
17 prepared real-media inputs; the large source video can be downloaded again
using its recorded URL/hash. It also includes `reference-source` separately,
at the stated baseline commit plus **only** the Linux CLI portability patch,
so a comparison build can be made without Git history. Recorded Windows
baseline timings used the untouched original, not that patched CLI.

### RK3588 build and board reproduction

For a native Linux build on the board, use the normal CMake instructions above.
For a GNU AArch64 cross toolchain:

```sh
cmake -S . -B build-rk3588 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588-linux.cmake \
  -DCMAKE_BUILD_TYPE=Release -DJXS_ENABLE_OPENMP=ON
cmake --build build-rk3588 --parallel 4
```

Set `CROSS_COMPILE` to your SDK's compiler prefix and `CMAKE_SYSROOT` when needed.
The toolchain uses common Armv8-A instructions with Cortex-A76 scheduling;
it does not assume all eight cores are A76, use an NPU/VPU, or claim hand-written
NEON acceleration. The cross-build configuration is provided but has not been
verified with the user's SDK or an AArch64 toolchain in this environment.
Deploy the matching target `libgomp` runtime and check it on the actual board.

```sh
bash extras/benchmark_rk3588.sh \
  build-original/bin build-parallel/bin test-data benchmark-results/board-all
```

The runner refuses non-RK3588 hardware and records model, topology/capacity,
governor, frequency and temperature snapshots without changing system settings.
Identify big/little CPU IDs from the actual board; do not assume numbering.
Repeat separately with `CPU_LIST` set to verified big-core IDs, little-core IDs
and all allowed CPUs. `THREADS`, `REPEATS` and `WARMUPS` are configurable.
Use longer runs to investigate thermal throttling; before/after snapshots are
not continuous thermal or power profiling.

## Introduction

This source code package represents an integral part of ISO/IEC 21122-5 Reference Software. It provides the reference software  implementation of the ISO/IEC 21122 series (JPEG XS). The code has been successfully compiled and tested on Linux, Windows, and MacOS operating systems at the time of writing.

It is important to note that this reference software implementation represents just one way of implementing JPEG XS. Alternative implementations that comply to the ISO/IEC 21122 standards are possible and equally valid. This implementation can serve as a validation anchor to other implementations.

No guarantee of the coding quality and performance that will be achieved by an encoder is provided by its conformance to ISO/IEC 21122-1, as the conformance is only defined in terms of specific constraints imposed on the syntax of the generated codestream. In particular, while sample encoder software implementations may suffice to provide some illustrative examples of which quality can be achieved within the ISO/IEC 21122 series, they provide neither an assurance of minimum guaranteed image encoding quality nor maximum achievable image encoding quality.

Similarly, the computation resource characteristics in terms of program or data memory usage, execution speed, etc of sample software encoder or decoder implementations should not be construed as a representative of the typical, minimal or maximal computational resource characteristics to be exhibited by implementations of ISO/IEC 21122-1.

Finally, despite the fact that this implementation was tested to conform to the ISO/IEC 21122 standards, it is possible that bugs or mistakes exist in the code. The texts of the ISO/IEC 21122-1, 21122-2 and 21122-3 standards remain authorative in case of any discrepancy.

## How to build

- This repository uses CMake (3.12 or newer) to generate the build files.

- The code was written to work with GCC (9.3) and Visual Studio (16.9) compilers, but will likely compile with many other compilers and versions.

- For Windows and Visual Studio 2015 or newer, use the ```cmake-gui``` on Windows to generate a solution.

- For Linux, the easiest way it to use the default GNU Make builder:
  `mkdir _build`
  `cd _build`
  `cmake ..`
  `make`

## How to use

### Decoding

The decoder takes a .jxs file and generates from that a decoded image (many formats are supported, like PNM, YUV, and PGX). To
run it, use ```jxs_decoder [options] <codestream.jxs> <output>```.

The decoder provides the following options:

- ```-D```: To dump the actual JPEG XS configuration as a string. The string is a ```;```-separated line of XS settings. This string can be used by the encoder to encode using the exact same configuration. Applying the option twice (```-DD``` or ```-D -D```) will also show the gain and priority values. Outputs in the same syntax as used by the encoder ```-c``` option.
- ```-f <number>```: Tells the decoder to interpret the input and output as a sequence of files (for frame-based video sequencing). The number specifies the first frame index to process. To be used in combination with ```-n```.
- ```-n <number>```: Specifies the total number number of frames to process.
- ```-v```: Output verbose. Use multiple times to increase the verbosity.

Note: When using the ```-D``` option to show the XS configuration of the codestream, it is optional to specify an output file name. In the case that no output is given, the decoder will not decode the codestream (after showing the XS configuration).

Example:

```
jxs_decoder -v woman.jxs out.ppm
```

### Encoding

The encoder is capable of reading various input file formats, such as PNM (PPM/PGM), PGX, raw YUV, and raw RGB. Please check the ISO/IEC 21122-5 text for more information. To run it, use ```jxs_encoder [options] <input> <codestream.jxs>```.

The encoder provides the following options:

- ```-c <XS config>```: XS configuration arguments (semicolon separated to specify multiple XS options, but can also be specified multiple times). See below for more on the XS configuration syntax.
- ```-D```: To dump the actual JPEG XS configuration as a string. The string is a ```;```-separated line of XS settings. This string can be used by the encoder to encode using the exact same configuration. Applying the option twice (```-DD``` or ```-D -D```) will also show the gain and priority values. Outputs in the same syntax as used by the ```-c``` option.
- ```-f <number>```: Tells the decoder to interpret the input and output as a sequence of files (for frame-based video sequencing). The number specifies the first frame index to process. To be used in combination with ```-n```.
- ```-n <number>```: Specifies the total number number of frames to process.
- ```-v```: Output verbose. Use multiple times to increase the verbosity.
- ```-w <width>```: Specifies the width of the input file (required for raw input format like v210, yuv16, rgb16).
- ```-h <height>```: Specifies the height of the input file (required for raw input format like v210, yuv16, rgb16).
- ```-d <bitdepth>```: Specifies the bit depth of the input file (required for raw input format like v210, yuv16, rgb16).

Note: For raw input file formats, the number of components is implicitly derived from the file extension.

Examples:

```
jxs_encoder -c profile=Main444.12 -c rate=2 woman.ppm woman.jxs
jxs_encoder -c "profile=Main444.12;rate=2" woman.ppm woman.jxs  # same, but combining the -c options
```

## The XS configuration syntax

The encoder ```-c``` option and the encoder/decoder ```-D``` options all use the same syntax to describe JPEG XS codestream configuration parameters. These parameters are mostly one-to-one representations of codestream configuration options as described in ISO 21122-1 and 21122-2 (a few represent implicit properties, such as the target bitrate, target codestream size, or the rate-allocation line buffer budget).

The profiles, levels and sublevels that can be used are defined in ISO/IEC 21122-2. Setting either one of these will impose specific XS configuration settings and limitations. For example, selecting the Main420.12 profile will trigger the encoder to require subsampled 4:2:0 input. The encoder validates the provided XS configuration under the given restrictions and limitations and will produce an error in the case of conflicting settings.

The generic syntax of the XS configuration is always as follows:

- Each option is specified as a ```key=value``` pair. The key is an alpha-numeric string and is well defined (see below).
- Options are separated by semi-colons (```;```). Multiple ```-c``` arguments are internally concatenated to produce a single XS configuration line comprised of key/value pairs.
- Values can be either text or number (integral or floating point), but a value is never the empty string.
- The XS configuration line is case insensitive (everything is converted to lower case).
- Whitespaces are ignored.
- By not specifying an XS option, the encoder will resolve to either a default value (as defined by the profile) or an automated best-effort guess.

The following XS configuration options (keys) are defined (for more information, see the ISO/IEC 21122-1 and -2 texts):

- ```p```/```profile```: The profile to use (by name). See ISO/IEC 21122-2.
- ```l```/```lev```/```level```: The level to use (by name). See ISO/IEC 21122-2.
- ```s```/```sublev```/```sublevel```: The sublevel to use (by name). See ISO/IEC 21122-2.
- ```rate```/```bpp```: Specify a target bit-rate (in bpp). Cannot be combined with ```size```.
- ```size```: Specify a target codestream size (in bytes). Cannot be combined with ```rate```.
- ```budget_lines```/```budget_report_lines```: The budget report buffer size (in lines) for the encoder's rate allocation.
- ```cpih```/```mct```: Select the multiple component transform. Valid options are ```none```/```0```, ```rct```/```1```, and ```tetrix,<Cf>,<e1>,<e2>```/```3,<Cf>,<e1>,<e2>```. Note that for the Star-Tetrix, additional settings are required (comma separated).
- ```cfa```: In case of 1-component input with one of the Bayer profiles, a CFA pattern needs to be specified. Supported patterns are ```RGGB```, ```BGGR```, ```GRBG```, and ```GBGR```.
- ```nlt```: Non-linear transform. Additional settings are required, depending on the chosen NLT. Valid NLT's are: ```none```, ```quadratic,<sigma>,<alpha>```, ```extended,<theta1>,<theta2>```, and ```extended,<E>,<T1>,<T2>```.
- ```cw```: Precinct column width in multiples of 8. ```Cw=0``` represents full-width.
- ```slh```: Slice height (in lines).
- ```bw```: Nominal bit precision of the wavelet coefficients (restricted to input-bit-depth, ```18```, or ```20```).
- ```fq```: Number of fractional bits of the wavelet coefficients (restricted to ```8```, ```6```, or ```0```).
- ```nlx```: Number of horizontal wavelet decompositions.
- ```nly```: Number of vertical wavelet decompositions (must be smaller than or equal to NLX).
- ```lh```: Long precinct header enforcement flag (```0``` or ```1```).
- ```rl```: Raw-mode selection per packet flag (```0``` or ```1```).
- ```qpih```/```quant```: Quantization type (use ```0``` for dead-zone quantization, and ```1``` for uniform quantization).
- ```fs```: Sign handling strategy (use ```0``` for jointly, and ```1``` for separate packets).
- ```rm```: Run mode (use ```0``` so runs indicate zero prediction residuals, and ```1``` so runs indicate zero coefficients).
- ```sd```: Number of components for which the wavelet decomposition is suppressed.
- ```gains```: Comma separated list of gain values (one value per band, which depends on number of components, subsampling and NLX/NLY). Alternatively, the encoder has some built-in tables for PSNR and Visual optimized compression. Specify either ```psnr``` or ```visual``` to select built-in gains/priorities.
- ```priorities```: Comma separated list of priority values (same amount as gains).

Some examples:

```jxsencoder -vv -DD -c "rate=4;lh=0;rl=1;gains=visual" sintel.ppm sintel.jxs```: Encodes ```sintel.ppm``` to 4 bits per pixel. Profile, level and sublevel are automatically selected. Long precinct header flag is disabled, raw-mode per packet flag is enabled, and matching built-in gains/priorities are applied for visual optimized quality. Verbosity is set to level 2. The double ```D``` option will also print out the full XS configuration string.

```jxsencoder -c "p=Main422.10" -c "rate=2" -w 1920 -h 1080 -d 10 sintel.yuv16_422p sintel.jxs```: Encodes a frame of sintel, given in YUV 422 10-bit format to 2 bits per pixel, using the Main422.10 profile.

Note: One particular useful way to learn and understand the XS configuration syntax and possibilities is to use the decoder to dump (using the ```-D``` option) configurations of the provided reference codestreams of ISO/IEC 21122-4.
