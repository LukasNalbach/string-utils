# string-utils

This repository contains C++20 headers, data structures, CMake functions and test helpers for programs that process
large strings and integer arrays, together with the third-party libraries they use.

- [include/misc/](include/misc/): file I/O with `O_DIRECT` and temporary files, logging of phases with time, memory and
  progress, measurement files, parallel sorting (ips4o, ips2ra), packed 24- to 56-bit integers, hashing, memory and
  thread helpers, a platform layer for Linux and Windows, and helpers for command-line tools.
- [include/rlbwt/](include/rlbwt/): the runs file of a run-length encoded BWT with SA and LCP samples (a plain and a
  compressed format), its writer and its readers (sequential, with random access on the disk and in RAM), and the
  texts a construction reads: files, FASTA files and texts in memory, with their bytes mapped to symbols that are at
  least a smallest symbol (the map is stored in the runs file).
- [include/fasta/](include/fasta/): FASTA files as texts (a parallel scan and a reader of the text), the split of a
  FASTA file into its sequences, header lines and line layout, its restoration, and a map from file positions to
  positions in the sequence text and back.
- [include/coding/](include/coding/): a binary arithmetic coder with context mixing and variable-length integers.
- [include/data_structures/](include/data_structures/): plain and Elias-Fano coded bit vectors with rank and select,
  interleaved bit-aligned vectors, a range minimum query tree and a segmented vector.
- [tests/](tests/): helpers for tests with random inputs (also random FASTA files), reference implementations of the
  suffix array, the LCP array and range minimum queries, and the tests of the FASTA code.
- [external/](external/): the submodules [sdsl-lite](https://github.com/simongog/sdsl-lite),
  [sux](https://github.com/vigna/sux), [malloc_count](https://github.com/ByteHamster-etc/malloc_count),
  [ips4o](https://github.com/ips4o/ips4o), [ips2ra](https://github.com/ips4o/ips2ra),
  [libsais](https://github.com/IlyaGrebnov/libsais), [gtl](https://github.com/greg7mdp/gtl),
  [emhash](https://github.com/ktprime/emhash) and [googletest](https://github.com/google/googletest).

The code builds on Linux with GCC or Clang, on macOS with Apple Clang and on Windows with Clang or MinGW-w64.

## Build

```shell
git clone https://github.com/LukasNalbach/string-utils.git
cd string-utils
cmake -B build -G Ninja
cmake --build build
```

`cmake` downloads the submodules and applies the patches in [patches/](patches/) to them. The libraries are built, the
tests (`build/tests/test-string-utils`) only with `-DSTRING_UTILS_BUILD_TESTS=ON`.

### macOS

Apple Clang has no OpenMP runtime, so install LLVM's `libomp` and the build tools with [Homebrew](https://brew.sh)
(Xcode or the Command Line Tools, `xcode-select --install`, provide the compiler):

```shell
brew install libomp cmake ninja
```

Then build as above. CMake finds `libomp` in Homebrew (`brew --prefix libomp`) or MacPorts (`port install libomp`);
for another installation, pass its prefix with `-DOpenMP_ROOT=<prefix>` or the environment variable `OpenMP_ROOT`.
TBB is not needed: ips4o and ips2ra use their queue of jobs without TBB, as on Windows.

Differences from Linux: macOS cannot bind threads to CPUs (the affinity functions do nothing), files opened with
`O_DIRECT` on Linux are opened with `F_NOCACHE`, the page cache cannot be dropped and transparent huge pages are not
used. If linking fails with `ld: tapi error: malformed file ... unknown architecture`, the Command Line Tools are older
than the newest SDK; update them, or select an older SDK from `/Library/Developer/CommandLineTools/SDKs/`, e.g.
`export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk`, before running `cmake`.

## Usage in CMake

A project adds string-utils as a submodule under `external/string-utils`. If several projects in one build add it,
the copy added first is used:

```cmake
set(STRING_UTILS_PREFIX MY_PROJECT)  # prefix of the options and environment variables

if(NOT TARGET string_utils)
  if(PROJECT_IS_TOP_LEVEL)
    include("${CMAKE_CURRENT_SOURCE_DIR}/external/string-utils/cmake/settings.cmake")
  endif()
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/string-utils" EXCLUDE_FROM_ALL)
endif()

string_utils_add_components(libsais)  # libraries beyond the core

target_link_libraries(<target> PRIVATE string_utils libsais40)
```

The target `string_utils` contains the headers and the libraries they need. `string_utils_add_components` adds further
libraries; only their submodules are downloaded:

| Component | Targets |
| --- | --- |
| `sdsl` | `sdsl`, all of sdsl-lite |
| `libsais` | `libsais` and its 40-bit and 48-bit variants `libsais40` and `libsais48` |
| `gtl` | `gtl` |
| `emhash` | `emhash` |
| `tests` | `string_utils_test`: the test helpers with googletest |

## Options and environment variables

The options and environment variables start with the prefix of the top-level project (`STRING_UTILS_PREFIX`, default
`STRING_UTILS`), e.g. `MY_PROJECT_MARCH_NATIVE` and `MY_PROJECT_TMPDIR`.

| Option | Default | Description |
| --- | --- | --- |
| `<prefix>_USE_MALLOC_COUNT` | `ON` | measure the peak memory usage with malloc_count |
| `<prefix>_EXTERN_TEMPLATES` | `ON` | compile the template instantiations once in a library |
| `<prefix>_MARCH_NATIVE` | `ON` | compile for the host CPU (`-march=native`) |
| `<prefix>_ENABLE_LTO` | `OFF` | link-time optimization of the executables |
| `<prefix>_FAST_TESTS` | `OFF` | compile the tests with `-O0` |
| `<prefix>_USE_CCACHE` | `ON` | use ccache, if it is installed |

| Environment variable | Description |
| --- | --- |
| `<prefix>_TMPDIR` | the directory for temporary files (default: the temporary directory of the system) |
| `<prefix>_TEST_SECONDS`, `<prefix>_TEST_ITERATIONS` | the maximum time in seconds (default 10) and number of random inputs (default 1000) per tested function |
| `<prefix>_TEST_SEED` | the seed printed by a failed test; the test is repeated with the random inputs of this seed |

## Patches

The changes to a submodule are stored in `patches/<submodule>.patch`; `cmake` applies them. After changing a file of a
submodule, `patches/make-patch.sh <submodule> [<new file>...]` updates its patch (from a project that uses
string-utils: `external/string-utils/patches/make-patch.sh`).

## License

This project is licensed under the MIT License, see [LICENSE](LICENSE). The libraries in [external/](external/) have
their own licenses (sdsl-lite: GPL-3.0).
