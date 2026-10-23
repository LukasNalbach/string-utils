# the prefix of the options and of the environment variables, and the CMake functions of string-utils; included by
# cmake/settings.cmake and by the CMakeLists.txt of string-utils

include_guard(GLOBAL)

find_package(Git REQUIRED)

# the prefix of the options (<prefix>_MARCH_NATIVE, ...) and of the environment variables (<prefix>_TMPDIR, ...); a
# top-level project sets it before it includes cmake/settings.cmake, e.g. set(STRING_UTILS_PREFIX MY_PROJECT)
if(NOT DEFINED STRING_UTILS_PREFIX)
  set(STRING_UTILS_PREFIX STRING_UTILS)
endif()

# string_utils_option(<name> <description> <default>)
#
# defines the option <prefix>_<name> and stores its value in the internal cache variable STRING_UTILS_<name>, which
# the CMake files of string-utils and of the projects that use it read
function(string_utils_option name description default)
  option(${STRING_UTILS_PREFIX}_${name} "${description}" ${default})

  if(NOT STRING_UTILS_PREFIX STREQUAL "STRING_UTILS")
    set(STRING_UTILS_${name} "${${STRING_UTILS_PREFIX}_${name}}" CACHE INTERNAL "${description}" FORCE)
  endif()
endfunction()

# string_utils_fetch_submodules(<project dir> [RECURSIVE] PATHS <path>...)
#
# downloads each of the given submodules of the project in <project dir> whose directory is empty (also
# submodules with update = none); RECURSIVE also downloads their submodules (use it only for third-party
# libraries)
function(string_utils_fetch_submodules project_dir)
  cmake_parse_arguments(PARSE_ARGV 1 arg "RECURSIVE" "" "PATHS")
  set(missing "")

  foreach(path IN LISTS arg_PATHS)
    file(GLOB contents "${project_dir}/${path}/*")

    if(contents STREQUAL "")
      list(APPEND missing "${path}")
    endif()
  endforeach()

  if(NOT missing STREQUAL "")
    set(recursive "")

    if(arg_RECURSIVE)
      set(recursive "--recursive")
    endif()

    message(STATUS "Downloading the git submodules: ${missing}")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" submodule update --init --checkout ${recursive} -- ${missing}
      WORKING_DIRECTORY "${project_dir}")
  endif()
endfunction()

# string_utils_add_components(<component>...)
#
# adds the libraries of string-utils beyond the core (the target string_utils) and downloads their submodules; a
# project calls it after string-utils has been added, for each library it links (a component that has already been
# added is skipped):
#   sdsl      sdsl: all of sdsl-lite (the core has the sources its headers need) with libdivsufsort (divsufsort,
#             divsufsort64)
#   libsais   libsais, libsais40 and libsais48 (the 40-bit and the 48-bit variant)
#   gtl       gtl
#   emhash    emhash
#   tests     string_utils_test: the test helpers (tests/) and googletest; a test that includes test-references.hpp
#             also links libsais and gtl
function(string_utils_add_components)
  set(source_dir "${STRING_UTILS_SOURCE_DIR}")
  set(external_dir "${STRING_UTILS_EXTERNAL_DIR}")
  set(binary_dir "${STRING_UTILS_BINARY_DIR}")

  foreach(component IN LISTS ARGN)
    if(component STREQUAL "sdsl")
      if(TARGET sdsl)
        continue()
      endif()

      set(sdsl_root "${external_dir}/sdsl-lite")
      string_utils_fetch_submodules("${sdsl_root}" PATHS external/libdivsufsort)

      # libdivsufsort (bundled with sdsl-lite)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
      set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(BUILD_DIVSUFSORT64 ON CACHE BOOL "" FORCE)
      set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
      set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
      add_subdirectory("${sdsl_root}/external/libdivsufsort" "${binary_dir}/external/libdivsufsort" EXCLUDE_FROM_ALL)
      unset(CMAKE_POLICY_VERSION_MINIMUM)
      set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)
      set(divsufsort_include_dir "${binary_dir}/external/libdivsufsort/include")
      set_target_properties(divsufsort divsufsort64 PROPERTIES POSITION_INDEPENDENT_CODE ON)
      target_include_directories(divsufsort INTERFACE "${divsufsort_include_dir}")
      target_include_directories(divsufsort64 INTERFACE "${divsufsort_include_dir}")

      # the sources of sdsl that are not in sdsl_core
      file(GLOB sdsl_sources "${sdsl_root}/lib/*.cpp")
      list(TRANSFORM STRING_UTILS_SDSL_CORE_SOURCES PREPEND "${sdsl_root}/lib/" OUTPUT_VARIABLE sdsl_core_sources)
      list(REMOVE_ITEM sdsl_sources ${sdsl_core_sources})
      add_library(sdsl STATIC EXCLUDE_FROM_ALL ${sdsl_sources})
      set_target_properties(sdsl PROPERTIES POSITION_INDEPENDENT_CODE ON)
      target_compile_options(sdsl PRIVATE -w -DNDEBUG -fpermissive)
      target_include_directories(sdsl PRIVATE "${divsufsort_include_dir}")
      target_link_libraries(sdsl PUBLIC sdsl_core divsufsort divsufsort64)
      string_utils_mark_system(divsufsort divsufsort64)
      string_utils_silence_warnings(divsufsort divsufsort64)
    elseif(component STREQUAL "libsais")
      if(TARGET libsais)
        continue()
      endif()

      set(libsais_root "${external_dir}/libsais")
      string_utils_fetch_submodules("${source_dir}" PATHS external/libsais)
      string_utils_apply_patches("${source_dir}")

      add_library(libsais STATIC EXCLUDE_FROM_ALL "${libsais_root}/src/libsais.c" "${libsais_root}/src/libsais16.c"
        "${libsais_root}/src/libsais16x64.c" "${libsais_root}/src/libsais64.c")
      set_target_properties(libsais PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
      target_include_directories(libsais PUBLIC "${libsais_root}/include")
      target_compile_definitions(libsais PUBLIC LIBSAIS_OPENMP)
      target_link_libraries(libsais PUBLIC OpenMP::OpenMP_C)

      add_library(libsais40 STATIC EXCLUDE_FROM_ALL "${libsais_root}/src/libsais40.cpp"
        "${libsais_root}/src/libsais16x40.cpp")
      target_include_directories(libsais40 PUBLIC "${libsais_root}/include" "${source_dir}/include")
      target_compile_definitions(libsais40 PUBLIC LIBSAIS_OPENMP)
      target_compile_options(libsais40 PRIVATE -fno-strict-aliasing)
      target_link_libraries(libsais40 PUBLIC OpenMP::OpenMP_CXX)
      set_target_properties(libsais40 PROPERTIES POSITION_INDEPENDENT_CODE ON)

      # libsais40.cpp with 48-bit integers (libsais48_impl, see libsais40.h)
      add_library(libsais48 STATIC EXCLUDE_FROM_ALL "${libsais_root}/src/libsais40.cpp")
      target_include_directories(libsais48 PUBLIC "${libsais_root}/include" "${source_dir}/include")
      target_compile_definitions(libsais48 PUBLIC LIBSAIS_OPENMP PRIVATE LIBSAIS_PACKED_BYTES=6)
      target_compile_options(libsais48 PRIVATE -fno-strict-aliasing)
      target_link_libraries(libsais48 PUBLIC OpenMP::OpenMP_CXX)
      set_target_properties(libsais48 PROPERTIES POSITION_INDEPENDENT_CODE ON)
      string_utils_mark_system(libsais)
      string_utils_silence_warnings(libsais)
    elseif(component STREQUAL "gtl")
      if(TARGET gtl)
        continue()
      endif()

      string_utils_fetch_submodules("${source_dir}" PATHS external/gtl)
      add_library(gtl INTERFACE)
      target_include_directories(gtl INTERFACE "${external_dir}/gtl/include")
      string_utils_mark_system(gtl)
    elseif(component STREQUAL "emhash")
      if(TARGET emhash)
        continue()
      endif()

      string_utils_fetch_submodules("${source_dir}" PATHS external/emhash)
      add_library(emhash INTERFACE)
      target_include_directories(emhash INTERFACE "${external_dir}/emhash/")
      string_utils_mark_system(emhash)
    elseif(component STREQUAL "tests")
      if(TARGET string_utils_test)
        continue()
      endif()

      string_utils_fetch_submodules("${source_dir}" PATHS external/googletest)
      add_subdirectory("${external_dir}/googletest" "${binary_dir}/external/googletest" EXCLUDE_FROM_ALL)
      set_target_properties(gtest gtest_main gmock gmock_main PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
      string_utils_mark_system(gtest gtest_main gmock gmock_main)
      string_utils_silence_warnings(gtest gtest_main gmock gmock_main)

      add_library(string_utils_test INTERFACE)
      target_include_directories(string_utils_test INTERFACE "${source_dir}/tests/")
      target_link_libraries(string_utils_test INTERFACE string_utils gtest_main)

      if(STRING_UTILS_FAST_TESTS)
        target_compile_options(string_utils_test INTERFACE -O0)
      endif()
    else()
      message(FATAL_ERROR "string_utils_add_components: unknown component ${component}")
    endif()
  endforeach()
endfunction()

# string_utils_apply_patches(<project dir>)
#
# applies each patch <project dir>/patches/<path below external/>.patch (written by patches/make-patch.sh
# of string-utils) to its submodule; a patch that has already been applied is skipped, and a patch that cannot
# be applied is skipped with a warning
function(string_utils_apply_patches project_dir)
  file(GLOB_RECURSE patches RELATIVE "${project_dir}/patches" "${project_dir}/patches/*.patch")

  foreach(patch IN LISTS patches)
    string(REGEX REPLACE "\\.patch$" "" submodule "${patch}")
    set(submodule_dir "${project_dir}/external/${submodule}")
    file(GLOB contents "${submodule_dir}/*")

    # skip submodules that have not been downloaded
    if(contents STREQUAL "")
      continue()
    endif()

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${project_dir}/patches/${patch}"
      WORKING_DIRECTORY "${submodule_dir}" RESULT_VARIABLE not_applied OUTPUT_QUIET ERROR_QUIET)

    if(not_applied)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${project_dir}/patches/${patch}"
        WORKING_DIRECTORY "${submodule_dir}" RESULT_VARIABLE failed OUTPUT_QUIET ERROR_VARIABLE error)

      if(failed)
        message(WARNING "patches/${patch} cannot be applied to external/${submodule} and has not been "
          "applied yet; the submodule is left unchanged (update the patch with make-patch.sh of "
          "string-utils):\n${error}")
      endif()
    endif()
  endforeach()
endfunction()

# string_utils_add_executable(<name> <sources> <output dir> <library>...)
#
# adds an executable linked against the given libraries; enables LTO and copies the Windows runtime
# libraries according to cmake/settings.cmake; on Windows, the executable gets the manifest cmake/windows.manifest
function(string_utils_add_executable name src outdir)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE ${ARGN})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${outdir}")

  # the MSVC ABI: CMake merges the manifest into the one it embeds with the linker; MinGW: a resource (windows.rc)
  if(WIN32 AND NOT MINGW)
    target_sources(${name} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows.manifest")
  elseif(WIN32 AND CMAKE_RC_COMPILER)
    target_sources(${name} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows.rc")
  endif()

  if(STRING_UTILS_ENABLE_LTO AND STRING_UTILS_IPO_SUPPORTED)
    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
  endif()

  foreach(_dll ${STRING_UTILS_WIN_RUNTIME_DLLS})
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "${outdir}")
  endforeach()
endfunction()

# string_utils_mark_system(<target>...): marks the include directories of third-party targets as system
# directories (suppresses the warnings from their headers)
function(string_utils_mark_system)
  foreach(tgt ${ARGN})
    if(TARGET ${tgt})
      get_target_property(includes ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
      if(includes)
        set_target_properties(${tgt} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${includes}")
      endif()
    endif()
  endforeach()
endfunction()

# string_utils_silence_warnings(<target>...): disables the warnings when compiling third-party targets
function(string_utils_silence_warnings)
  foreach(tgt ${ARGN})
    if(TARGET ${tgt})
      get_target_property(type ${tgt} TYPE)
      if(NOT type STREQUAL "INTERFACE_LIBRARY")
        if(MSVC)
          target_compile_options(${tgt} PRIVATE /w)
        else()
          target_compile_options(${tgt} PRIVATE -w)
        endif()
      endif()
    endif()
  endforeach()
endfunction()
