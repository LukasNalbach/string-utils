# string-utils

This repository contains C++20 headers, data structures, CMake functions and test helpers for programs that process large strings and integer arrays, together with the third-party libraries they use.

## Contents

- [include/misc/](include/misc/)
  - file I/O in blocks and with `O_DIRECT` (`files`), temporary files (`temp_files`)
  - logging of phases with time, memory usage, progress and the estimated remaining time (`log`, `aligned_table`), measurement files with `RESULT` lines (`measurement`)
  - parallel and sequential sorting with ips4o and ips2ra (`sort`)
  - bit operations (`bits`), packed 40- and 48-bit integers (`packed_integers`), struct layouts that are the same on Linux and Windows (`portable_layout`), compile-time helpers (`type_utils`), common types (`utils`)
  - splitmix64 and Karp-Rabin fingerprints modulo 2^61 - 1 (`hashing`)
  - allocation without initialization, cache-aligned allocation and huge pages (`memory`), thread affinity and concurrent tasks with OpenMP (`threads`)
  - the platform layer for Linux and Windows: unbuffered and positioned file I/O, page cache, CPUs, memory, console and environment (`platform`, with [src/platform_windows.cpp](src/platform_windows.cpp))
  - command-line tools: help output, argument parsing and error handling (`cli_help`), locale-independent number parsing (`parse`), the command-line interface of a compressor with an optional FASTA mode (`compressor_cli`), extraction of text ranges from a compressed representation (`extraction`) and general-purpose postcompressors run as external programs (`postcompressors`)
  - generation of repetitive test inputs (`repetitive_input`)
- [include/coding/](include/coding/) — a binary arithmetic coder (range coder) with adaptive bit models (`range_coder`), logistic mixing of the predictions of bit models and values coded bit by bit along binary trees (`mixer`), context models of integers and decisions (`integer_model`), and variable-length integers and bit-packed values (`byte_codes`).
- [include/fasta/](include/fasta/) — FASTA files as texts: the transformation of the bytes of a FASTA file into the symbols of a text, the parallel scan with samples of the text and a reader of the text that starts at the samples (`scan`); the split of a FASTA file into its header lines and its line layout, coded with context models (`header_model`, `layout_model`), and its sequence text (`split`), and the restoration of the file (`restore`), see [FASTA Files](#fasta-files).
- [include/data_structures/](include/data_structures/) — a plain and an Elias-Fano coded bit vector with rank and select support, interleaved bit-aligned vectors, a range minimum query tree and a segmented vector.
- [tests/](tests/) — test helpers: random inputs within a time or iteration budget (`test-fuzz.hpp`), test inputs (`test-inputs.hpp`) and reference implementations of the suffix array, the LCP array and range minimum queries (`test-references.hpp`).
- [external/](external/) — the submodules [sdsl-lite](https://github.com/simongog/sdsl-lite), [sux](https://github.com/vigna/sux), [malloc_count](https://github.com/ByteHamster-etc/malloc_count), [ips4o](https://github.com/ips4o/ips4o), [ips2ra](https://github.com/ips4o/ips2ra), [libsais](https://github.com/IlyaGrebnov/libsais) (also built as the 40-bit variant `libsais40`), [gtl](https://github.com/greg7mdp/gtl), [emhash](https://github.com/ktprime/emhash) and [googletest](https://github.com/google/googletest), with the patches in [patches/](patches/).
- [cmake/](cmake/) — the build settings (`settings.cmake`) and the CMake functions (`string_utils.cmake`).

The code builds on Linux with GCC or Clang and on Windows with Clang (MSVC ABI) or MinGW-w64.

## Build

```shell
git clone https://github.com/LukasNalbach/string-utils.git
cd string-utils
cmake -B build -G Ninja
cmake --build build
```

`cmake` downloads missing submodules and applies the patches in [patches/](patches/) to them. The build creates the libraries (`string_utils`, `libsais40`, `sdsl`, ...); there are no programs.

## Usage in CMake

A project adds string-utils as a submodule under `external/string-utils`. If several projects in one build add string-utils, only the copy added first is used:

```cmake
set(STRING_UTILS_PREFIX MY_PROJECT)  # prefix of the options and
                                     # environment variables

if(NOT TARGET string_utils)
  if(PROJECT_IS_TOP_LEVEL)
    include("${CMAKE_CURRENT_SOURCE_DIR}/external/string-utils/cmake/settings.cmake")
  endif()
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/string-utils")
endif()

target_link_libraries(<target> PRIVATE string_utils)  # headers and libraries
target_link_libraries(<test> PRIVATE string_utils_test)  # also the test
                                                         # helpers and googletest
```

`settings.cmake` sets the C++ standard, the compiler flags of the build types and the Windows toolchain settings; only the top-level project includes it. `string_utils.cmake` provides the functions `string_utils_option`, `string_utils_fetch_submodules`, `string_utils_apply_patches`, `string_utils_add_executable`, `string_utils_mark_system` and `string_utils_silence_warnings`.

## Options and environment variables

The options and environment variables carry the prefix `STRING_UTILS_PREFIX` (default: `STRING_UTILS`), which the top-level project sets before it includes `settings.cmake`. A project with the prefix `MY_PROJECT` thus has the options `MY_PROJECT_MARCH_NATIVE`, ... and reads the environment variables `MY_PROJECT_TMPDIR`, ...; the CMake files read the value of each option `<prefix>_<name>` from `STRING_UTILS_<name>`.

| Option | Default | Description |
| --- | --- | --- |
| `<prefix>_USE_MALLOC_COUNT` | `ON` | measure the peak memory usage with malloc_count (overrides malloc and free) |
| `<prefix>_EXTERN_TEMPLATES` | `ON` | compile the template instantiations once in a library instead of in every translation unit |
| `<prefix>_MARCH_NATIVE` | `ON` | compile for the host CPU (`-march=native`); `OFF` for portable binaries |
| `<prefix>_ENABLE_LTO` | `OFF` | link-time optimization of the executables |
| `<prefix>_FAST_TESTS` | `OFF` | compile the tests (the targets that link `string_utils_test`) with `-O0` |
| `<prefix>_USE_CCACHE` | `ON` | use ccache, if it is installed |

| Environment variable | Description |
| --- | --- |
| `<prefix>_TMPDIR` | directory of the temporary files (default: the temporary directory of the system) |
| `<prefix>_HUGE_PAGES` | `0`: no advice, `< 0`: no huge pages, else (default): huge pages for large allocations |
| `<prefix>_TEST_SECONDS` | maximum time in seconds per tested functionality (default 10) |
| `<prefix>_TEST_ITERATIONS` | maximum number of random inputs per tested functionality (default 1000) |
| `<prefix>_TEST_SEED` | replays the random inputs of a seed that a failed test printed |

## FASTA Files

`split_fasta` (`fasta/split.hpp`) scans a FASTA file once in parallel and splits it into three parts; `fasta_restorer` (`fasta/restore.hpp`) writes the file from them byte for byte, also a file that is not a FASTA file:

- the sequence text: the bytes of the sequence lines of each record without their line ends, with a line feed between the sequences of consecutive records (lowercase letters, `N` runs and other bytes stay in it); it is not written anywhere, `fasta_text_reader` reads it from the FASTA file, starting at the samples of the scan;
- the header lines: they are split into tokens (runs of digits, runs of letters, single other bytes); each token is coded with a range coder and context mixing relative to the token at the same position of the previous header: equal, a number with a coded difference, or new;
- the line layout: per record the line width, the lines that deviate from it and the empty lines at its end; the line end (`\n` or `\r\n`) and a missing line end at the end of the file are flags.

A line starting with `>` is a header line, every other line is a sequence line of the current record; lines before the first header line form a record without a header. The FASTA section of a compressed file (`fasta_section`) stores the flags (one byte: bit 0 = the lines end with `\r\n`, bit 1 = the last line has no line end, bit 2 = the file does not start with a header line), the number of records, the size of the coded headers and the size of the coded line layout in bytes (variable-length integers, 7 bits per byte, the lowest group first, the highest bit set in each byte except the last), then the coded headers and the coded line layout. `compressor_cli` adds the option `-fasta on|off|auto` to a compressor; `decompress_parsing` restores a file whose first byte is the mode (0: the text as it is, 1: a FASTA file, followed by its FASTA section and the parsing of its sequence text).

## Patches

The changes made to a submodule are stored in `patches/<path below external/>.patch` and are applied by CMake when configuring (`string_utils_apply_patches`). A patch that has already been applied is skipped. After changing a file of a submodule, update its patch from the root of the project the patch belongs to:

```shell
# in string-utils
patches/make-patch.sh <submodule> [<new file>...]
# in a project that uses string-utils
external/string-utils/patches/make-patch.sh <submodule> [<new file>...]
```

The patch contains the files it contained before and the new files given.

## License

This project is licensed under the MIT License, see [LICENSE](LICENSE). The libraries in [external/](external/) keep their own licenses (sdsl-lite: GPL-3.0).
