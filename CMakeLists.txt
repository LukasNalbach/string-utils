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

# the submodules of the core (string_utils); the others are downloaded by string_utils_add_components
string_utils_fetch_submodules("${CMAKE_CURRENT_SOURCE_DIR}" PATHS
  external/sdsl-lite external/sux external/malloc_count external/ips4o external/ips2ra)
string_utils_apply_patches("${CMAKE_CURRENT_SOURCE_DIR}")

set(STRING_UTILS_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external" CACHE INTERNAL
  "directory of the third-party libraries of string-utils")
set(STRING_UTILS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "source directory of string-utils")
set(STRING_UTILS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE INTERNAL "build directory of string-utils")

############################# third-party libraries #############################

# openmp (the imported targets are made global, since the projects that use string-utils link them)
find_package(OpenMP REQUIRED)
foreach(openmp OpenMP::OpenMP_CXX OpenMP::OpenMP_C)
  if(TARGET ${openmp})
    set_target_properties(${openmp} PROPERTIES IMPORTED_GLOBAL TRUE)
  endif()
endforeach()

# MinGW: the static libgomp calls dlopen and dlsym, which MinGW's libdl provides (linked after libgomp)
if(MINGW)
  foreach(openmp OpenMP::OpenMP_CXX OpenMP::OpenMP_C)
    if(TARGET ${openmp})
      set_property(TARGET ${openmp} APPEND PROPERTY INTERFACE_LINK_LIBRARIES dl)
    endif()
  endforeach()
endif()

# ips4o (on Windows with a queue of jobs without TBB, see patches/ips4o.patch)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/ips4o" EXCLUDE_FROM_ALL)

if(WIN32)
  target_compile_definitions(ips4o INTERFACE IPS4O_NO_TBB)
endif()

# ips2ra (its parallel sort is compiled through _REENTRANT; on Windows with a queue of jobs without TBB, see
# patches/ips2ra.patch)
set(IPS2RA_DISABLE_PARALLEL ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/ips2ra" EXCLUDE_FROM_ALL)

if(WIN32)
  target_compile_definitions(ips2ra INTERFACE IPS2RA_NO_TBB)
endif()

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

# the sources of sdsl the headers of string-utils need (the others are in the library sdsl, see
# string_utils_add_components)
set(SDSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/sdsl-lite")
set(STRING_UTILS_SDSL_CORE_SOURCES bits.cpp bp_support_algorithm.cpp memory_management.cpp ram_fs.cpp util.cpp
  CACHE INTERNAL "the sources of sdsl in sdsl_core, in ${SDSL_ROOT}/lib/")
list(TRANSFORM STRING_UTILS_SDSL_CORE_SOURCES PREPEND "${SDSL_ROOT}/lib/" OUTPUT_VARIABLE sdsl_core_sources)
add_library(sdsl_core STATIC EXCLUDE_FROM_ALL ${sdsl_core_sources})
set_target_properties(sdsl_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(sdsl_core PRIVATE -w -DNDEBUG -fpermissive)
target_include_directories(sdsl_core PUBLIC "${SDSL_ROOT}/include")

# sux
add_library(sux INTERFACE)
target_include_directories(sux INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/external/sux/")

############################# string-utils #############################

set(string_utils_dependencies OpenMP::OpenMP_CXX sdsl_core ips4o ips2ra malloc_count sux)

add_library(string_utils INTERFACE)
target_include_directories(string_utils INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
target_compile_definitions(string_utils INTERFACE STRING_UTILS_ENV_PREFIX="${STRING_UTILS_PREFIX}")
target_link_libraries(string_utils INTERFACE ${string_utils_dependencies})

if(STRING_UTILS_EXTERN_TEMPLATES)
  # explicit instantiations of the sort functions (see misc/sort.hpp)
  add_library(string_utils_sort STATIC EXCLUDE_FROM_ALL src/sort.cpp)
  target_include_directories(string_utils_sort PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
  target_link_libraries(string_utils_sort PRIVATE ${string_utils_dependencies})
  target_link_libraries(string_utils INTERFACE string_utils_sort)
else()
  target_compile_definitions(string_utils INTERFACE STRING_UTILS_NO_EXTERN_TEMPLATES)
endif()

# the Windows functions of misc/platform.hpp
if(WIN32)
  add_library(string_utils_platform STATIC EXCLUDE_FROM_ALL src/platform_windows.cpp)
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

string_utils_mark_system(sdsl_core ips4o ips2ra sux malloc_count)
string_utils_silence_warnings(sdsl_core ips4o ips2ra sux malloc_count)

# a build of string-utils alone builds all of its libraries
if(PROJECT_IS_TOP_LEVEL)
  string_utils_add_components(sdsl libsais gtl emhash tests)
  add_custom_target(string_utils_libraries ALL)
  add_dependencies(string_utils_libraries sdsl_core sdsl libsais libsais40 libsais48 divsufsort divsufsort64)

  if(TARGET string_utils_sort)
    add_dependencies(string_utils_libraries string_utils_sort)
  endif()

  include(GoogleTest)
  enable_testing()
  string_utils_add_executable(test-string-utils tests/test-string-utils.cpp "${CMAKE_BINARY_DIR}/tests/"
    string_utils_test)
  set_target_properties(test-string-utils PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
  gtest_discover_tests(test-string-utils)
endif()
