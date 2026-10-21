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
