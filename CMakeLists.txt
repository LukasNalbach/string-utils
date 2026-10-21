cmake_minimum_required(VERSION 3.21)

# headers (include/), third-party libraries (external/, patched with patches/), test helpers (tests/) and CMake
# functions (cmake/string_utils.cmake) for C++ programs on strings; a project adds this directory only
# if(NOT TARGET string_utils)
project(string-utils LANGUAGES C CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/string_utils.cmake")

if(PROJECT_IS_TOP_LEVEL)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/settings.cmake")
endif()

string_utils_option(USE_MALLOC_COUNT "Track memory usage via malloc_count (overrides malloc/free)" ON)
string_utils_option(EXTERN_TEMPLATES "Compile the instantiation libraries once instead of in every translation unit" ON)

string_utils_fetch_submodules("${CMAKE_CURRENT_SOURCE_DIR}" RECURSIVE PATHS
  external/sdsl-lite external/sux external/malloc_count external/ips4o external/ips2ra
  external/libsais external/gtl external/emhash external/googletest)
string_utils_apply_patches("${CMAKE_CURRENT_SOURCE_DIR}")

set(STRING_UTILS_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external" CACHE INTERNAL
  "directory of the third-party libraries of string-utils")

############################# third-party libraries #############################

# openmp (the imported targets are made global, since the projects that use string-utils link them)
find_package(OpenMP REQUIRED)
foreach(openmp OpenMP::OpenMP_CXX OpenMP::OpenMP_C)
  if(TARGET ${openmp})
    set_target_properties(${openmp} PROPERTIES IMPORTED_GLOBAL TRUE)
  endif()
endforeach()

# ips4o (on Windows with a queue of jobs without TBB, see patches/ips4o.patch)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/ips4o")
set_target_properties(ips4o_example PROPERTIES EXCLUDE_FROM_ALL 1 EXCLUDE_FROM_DEFAULT_BUILD 1)

if(WIN32)
  target_compile_definitions(ips4o INTERFACE IPS4O_NO_TBB)
endif()

# ips2ra (its parallel sort is compiled through _REENTRANT; on Windows with a queue of jobs without TBB, see
# patches/ips2ra.patch)
set(IPS2RA_DISABLE_PARALLEL ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/ips2ra")

if(WIN32)
  target_compile_definitions(ips2ra INTERFACE IPS2RA_NO_TBB)
endif()

# libsais
set(LIBSAIS_USE_OPENMP ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/libsais/")

add_library(libsais40 STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/external/libsais/src/libsais40.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/external/libsais/src/libsais16x40.cpp"
)
target_include_directories(libsais40 PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/external/libsais/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(libsais40 PUBLIC LIBSAIS_OPENMP)
target_compile_options(libsais40 PRIVATE -fno-strict-aliasing)
target_link_libraries(libsais40 PUBLIC OpenMP::OpenMP_CXX)
set_target_properties(libsais40 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# malloc_count; on Windows src/malloc_count_windows.cpp, which replaces the heap functions of the C runtime: with the
# MSVC ABI only those of the static C runtime (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded, see cmake/settings.cmake)
add_library(malloc_count INTERFACE)
target_include_directories(malloc_count INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/external/malloc_count/")
if(STRING_UTILS_USE_MALLOC_COUNT AND NOT WIN32)
  target_sources(malloc_count INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/external/malloc_count/malloc_count.c")
  target_link_libraries(malloc_count INTERFACE dl)
  target_compile_definitions(malloc_count INTERFACE STRING_UTILS_USE_MALLOC_COUNT=1)
elseif(STRING_UTILS_USE_MALLOC_COUNT AND (MINGW OR CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded"))
  target_sources(malloc_count INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/malloc_count_windows.cpp")
  target_include_directories(malloc_count INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
  target_compile_definitions(malloc_count INTERFACE STRING_UTILS_USE_MALLOC_COUNT=1)
else()
  target_sources(malloc_count INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/malloc_count_stub.c")
  target_compile_definitions(malloc_count INTERFACE STRING_UTILS_USE_MALLOC_COUNT=0)
endif()

# MinGW: the static libgomp calls dlopen and dlsym, which MinGW's libdl provides (linked after libgomp); after the
# last find_package(OpenMP) (libsais calls it, too), which sets the libraries of the OpenMP targets anew
if(MINGW)
  foreach(openmp OpenMP::OpenMP_CXX OpenMP::OpenMP_C)
    if(TARGET ${openmp})
      set_property(TARGET ${openmp} APPEND PROPERTY INTERFACE_LINK_LIBRARIES dl)
    endif()
  endforeach()
endif()

# sdsl
set(SDSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/sdsl-lite")
set(SDSL_INCLUDE_DIR "${SDSL_ROOT}/include")

# libdivsufsort (bundled with sdsl-lite)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DIVSUFSORT64 ON CACHE BOOL "" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("${SDSL_ROOT}/external/libdivsufsort" "${CMAKE_CURRENT_BINARY_DIR}/external/libdivsufsort")
unset(CMAKE_POLICY_VERSION_MINIMUM)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)
set(DIVSUFSORT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/external/libdivsufsort/include")
set_target_properties(divsufsort divsufsort64 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(divsufsort INTERFACE "${DIVSUFSORT_INCLUDE_DIR}")
target_include_directories(divsufsort64 INTERFACE "${DIVSUFSORT_INCLUDE_DIR}")

# sdsl static library
file(GLOB sdsl_sources "${SDSL_ROOT}/lib/*.cpp")
add_library(sdsl STATIC ${sdsl_sources})
set_target_properties(sdsl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(sdsl PRIVATE -w -DNDEBUG -fpermissive)
target_include_directories(sdsl PUBLIC "${SDSL_INCLUDE_DIR}")
target_include_directories(sdsl PRIVATE "${DIVSUFSORT_INCLUDE_DIR}")
target_link_libraries(sdsl PUBLIC divsufsort divsufsort64)

# gtl
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/gtl/")

# emhash
add_library(emhash INTERFACE)
target_include_directories(emhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/external/emhash/")

# sux
add_library(sux INTERFACE)
target_include_directories(sux INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/external/sux/")

# googletest (only built if a test links it)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/googletest/" EXCLUDE_FROM_ALL)
set_target_properties(gtest gtest_main gmock gmock_main
  PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

############################# string-utils #############################

set(string_utils_dependencies
  OpenMP::OpenMP_CXX sdsl libsais libsais40 ips4o ips2ra gtl malloc_count emhash sux divsufsort divsufsort64)

add_library(string_utils INTERFACE)
target_include_directories(string_utils INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
target_compile_definitions(string_utils INTERFACE STRING_UTILS_ENV_PREFIX="${STRING_UTILS_PREFIX}")
target_link_libraries(string_utils INTERFACE ${string_utils_dependencies})

if(STRING_UTILS_EXTERN_TEMPLATES)
  # explicit instantiations of the sort functions (see misc/sort.hpp)
  add_library(string_utils_sort STATIC src/sort.cpp)
  target_include_directories(string_utils_sort PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
  target_link_libraries(string_utils_sort PRIVATE ${string_utils_dependencies})
  target_link_libraries(string_utils INTERFACE string_utils_sort)
else()
  target_compile_definitions(string_utils INTERFACE STRING_UTILS_NO_EXTERN_TEMPLATES)
endif()

# the Windows functions of misc/platform.hpp
if(WIN32)
  add_library(string_utils_platform STATIC src/platform_windows.cpp)
  target_include_directories(string_utils_platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
  target_link_libraries(string_utils_platform PRIVATE OpenMP::OpenMP_CXX)
  # advapi32: the privilege for large pages (see platform::enable_large_pages)
  target_link_libraries(string_utils_platform PUBLIC advapi32)

  # the structs of the Windows headers have the bit-field layout of Microsoft (see -mno-ms-bitfields in
  # cmake/settings.cmake)
  if(MINGW)
    target_compile_options(string_utils_platform PRIVATE -mms-bitfields)
    # the functions of Windows 10 (0x0A00 = _WIN32_WINNT_WIN10 of sdkddkver.h, the default of the Windows SDK; MinGW's
    # default is Windows 7)
    target_compile_definitions(string_utils_platform PRIVATE _WIN32_WINNT=0x0A00)
  endif()
  target_link_libraries(string_utils INTERFACE string_utils_platform)
endif()

if(MINGW)
  # libatomic: GCC calls it for the 16-byte compare-and-swap of ips4o
  target_link_libraries(string_utils INTERFACE atomic)
  # the destructors of thread_local objects run before their memory is freed (compiled into every executable)
  target_sources(string_utils INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/mingw_thread_atexit.cpp")
  target_link_options(string_utils INTERFACE -static)
endif()

# test helpers (tests/) and googletest
add_library(string_utils_test INTERFACE)
target_include_directories(string_utils_test INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests/")
target_link_libraries(string_utils_test INTERFACE string_utils gtest_main)

if(STRING_UTILS_FAST_TESTS)
  target_compile_options(string_utils_test INTERFACE -O0)
endif()

set(string_utils_external_targets
  sdsl divsufsort divsufsort64 libsais ips4o ips2ra gtl emhash sux malloc_count
  gtest gtest_main gmock gmock_main)

string_utils_mark_system(${string_utils_external_targets})
string_utils_silence_warnings(${string_utils_external_targets})
