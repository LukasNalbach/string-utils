# build settings; included only by the top-level project (they apply to all directories added by it), after it has
# set STRING_UTILS_PREFIX (see cmake/string_utils.cmake)

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/string_utils.cmake")

set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

message("building in ${CMAKE_BUILD_TYPE} mode")

string_utils_option(MARCH_NATIVE "Tune the build for the host CPU (-march=native); turn OFF for portable binaries" ON)
string_utils_option(ENABLE_LTO "Enable link-time optimization (LTO/IPO) for the executables" OFF)
string_utils_option(FAST_TESTS "Compile the tests with -O0" OFF)
string_utils_option(USE_CCACHE "Route the compiler through ccache when it is installed" ON)
set(STRING_UTILS_MAX_NARROW_VALUE "" CACHE STRING
  "Largest value stored with 40 bits (empty: 2^40 - 1); set a small value to test the 48-bit variants")

# the largest value stored with 40 bits, for all targets (see max_narrow_value in include/misc/packed_integers.hpp)
if(NOT STRING_UTILS_MAX_NARROW_VALUE STREQUAL "")
  add_compile_definitions(STRING_UTILS_MAX_NARROW_VALUE=${STRING_UTILS_MAX_NARROW_VALUE})
endif()

# ccache (not used if a compiler launcher is already set)
if(STRING_UTILS_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(STRING_UTILS_CCACHE ccache)
  if(STRING_UTILS_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${STRING_UTILS_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${STRING_UTILS_CCACHE}")
    message(STATUS "Using ccache: ${STRING_UTILS_CCACHE}")
  endif()
endif()

# Windows: clang++ with the GNU-like command line for the target x86_64-pc-windows-msvc (string_utils_msvc_abi), linked
# with lld against the static C runtime and LLVM's OpenMP runtime (libomp), or MinGW-w64 (GCC)
if(WIN32 AND NOT MINGW)
  set(string_utils_msvc_abi TRUE)
else()
  set(string_utils_msvc_abi FALSE)
endif()

# compile flags
if(STRING_UTILS_MARCH_NATIVE)
  set(string_utils_arch_flags "-march=native")
else()
  set(string_utils_arch_flags "")
endif()

if(NOT string_utils_msvc_abi)
  add_compile_options(-pthread)
endif()

# one section per function, such that the linker removes unused functions (--gc-sections, /OPT:REF)
add_compile_options(-ffunction-sections -fdata-sections)

if(string_utils_msvc_abi)
  add_link_options(LINKER:/OPT:REF,/OPT:ICF)
else()
  add_link_options(-Wl,--gc-sections)
endif()

# debug information: DWARF for gdb, CodeView (PDB files) for the MSVC ABI
if(string_utils_msvc_abi)
  set(string_utils_debug_info_flags "-g -gcodeview")
else()
  set(string_utils_debug_info_flags "-ggdb")
endif()

set(string_utils_debug_flags "-Wall -Wextra -Wpedantic ${string_utils_debug_info_flags}")
set(string_utils_release_flags "-w ${string_utils_arch_flags} -DNDEBUG")

set(CMAKE_CXX_FLAGS_DEBUG           "${string_utils_debug_flags} -O0")
set(CMAKE_CXX_FLAGS_RELEASE         "${string_utils_release_flags} -O3")
set(CMAKE_CXX_FLAGS_MINSIZEREL      "${string_utils_release_flags} -Os")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO  "${string_utils_debug_flags} -O3")

if(WIN32)
  # the resource compiler, for the manifest of the executables (see string_utils_add_executable in string_utils.cmake)
  enable_language(RC)

  # stand-ins for <sys/mman.h> (sux) and <sys/resource.h> (sdsl)
  include_directories(BEFORE "${CMAKE_CURRENT_LIST_DIR}/winshim")

  # stack size in bytes of the main thread and of the threads of OpenMP: the default of Linux (ulimit -s), where the
  # default of Windows is 1 MiB (the threads of OpenMP get it in platform::init, see include/misc/platform.hpp)
  math(EXPR STRING_UTILS_STACK_BYTES "8 * 1024 * 1024")
  add_compile_definitions(_REENTRANT STRING_UTILS_STACK_BYTES=${STRING_UTILS_STACK_BYTES})

  # the check of -march=native in the CMake files of ips4o and ips2ra fails on Windows (<prefix>_MARCH_NATIVE applies)
  set(IPS4O_OPTIMIZE_FOR_NATIVE OFF CACHE BOOL "" FORCE)
  set(IPS2RA_OPTIMIZE_FOR_NATIVE OFF CACHE BOOL "" FORCE)
  # the CMake file of ips4o would link TBB, Threads and libatomic; the parallel sort is compiled anyway (_REENTRANT),
  # with a queue of jobs without TBB (IPS4O_NO_TBB, see the CMakeLists.txt of string-utils)
  set(IPS4O_DISABLE_PARALLEL ON CACHE BOOL "" FORCE)
  # the 16-byte compare-and-swap of ips4o as an instruction (implied by -march=native)
  add_compile_options(-mcx16)

  # the tests are listed when ctest runs, not when they are built
  set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

  if(string_utils_msvc_abi)
    add_compile_definitions(MSVC_COMPILER _HAS_DEPRECATED_RESULT_OF=1 _CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_link_options(LINKER:/STACK:${STRING_UTILS_STACK_BYTES})

    if(NOT DEFINED CMAKE_LINKER_TYPE)
      set(CMAKE_LINKER_TYPE LLD)
    endif()

    # LLVM's builtins library, for the division of 128-bit integers (__udivti3, __umodti3)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --rtlib=compiler-rt --print-libgcc-file-name
      OUTPUT_VARIABLE string_utils_builtins_library OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    if(EXISTS "${string_utils_builtins_library}")
      link_libraries("${string_utils_builtins_library}")
    else()
      message(WARNING "LLVM's builtins library (clang_rt.builtins) was not found; the division of 128-bit integers "
        "cannot be linked")
    endif()

    # LLVM's OpenMP runtime (libomp.lib, libomp.dll next to each executable)
    get_filename_component(string_utils_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(string_utils_llvm_root "${string_utils_llvm_bin}" DIRECTORY)

    if(EXISTS "${string_utils_llvm_root}/lib/libomp.lib")
      foreach(lang C CXX)
        set(OpenMP_${lang}_FLAGS "-fopenmp=libomp" CACHE STRING "" FORCE)
        set(OpenMP_${lang}_LIB_NAMES "libomp" CACHE STRING "" FORCE)
      endforeach()

      set(OpenMP_libomp_LIBRARY "${string_utils_llvm_root}/lib/libomp.lib" CACHE FILEPATH "" FORCE)

      if(EXISTS "${string_utils_llvm_root}/bin/libomp.dll")
        list(APPEND STRING_UTILS_WIN_RUNTIME_DLLS "${string_utils_llvm_root}/bin/libomp.dll")
      endif()
    endif()
  else()
    add_link_options(-Wl,--stack,${STRING_UTILS_STACK_BYTES})

    # bit-fields laid out as on Linux (packed structs with 40- and 48-bit fields); the Windows functions of string-utils
    # keep the layout of the Windows headers (see the CMakeLists.txt of string-utils)
    add_compile_options(-mno-ms-bitfields)

    # GCC cannot align the stack of Windows to 32 or 64 bytes for spilled AVX registers (GCC bug 54412)
    if(STRING_UTILS_MARCH_NATIVE)
      add_compile_options(-Wa,-muse-unaligned-vector-move)
    endif()

    # libgomp is linked statically (its dlopen and dlsym come from MinGW's libdl, see the CMakeLists.txt of
    # string-utils)
    get_filename_component(string_utils_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(string_utils_mingw_root "${string_utils_mingw_bin}" DIRECTORY)

    if(EXISTS "${string_utils_mingw_root}/lib/libgomp.a")
      set(OpenMP_gomp_LIBRARY "${string_utils_mingw_root}/lib/libgomp.a" CACHE FILEPATH "" FORCE)
    endif()
  endif()
endif()

if(STRING_UTILS_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT STRING_UTILS_IPO_SUPPORTED OUTPUT STRING_UTILS_IPO_ERROR LANGUAGES CXX)
  if(NOT STRING_UTILS_IPO_SUPPORTED)
    message(WARNING "${STRING_UTILS_PREFIX}_ENABLE_LTO is ON but LTO/IPO is not supported by the toolchain: "
      "${STRING_UTILS_IPO_ERROR}")
  endif()
endif()
