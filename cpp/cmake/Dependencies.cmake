# Runtime libraries added by the desktop application. Source archives and
# hashes are deliberately fixed; Python is never involved in the build.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build bundled dependencies statically" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
set(USE_DTOA_LIBRARY ON CACHE BOOL "Locale-independent Excel numbers" FORCE)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
    URL_HASH SHA256=98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb)
FetchContent_Declare(xlsxwriter
    URL https://github.com/jmcnamara/libxlsxwriter/archive/refs/tags/v1.2.4.tar.gz
    URL_HASH SHA256=e0db59fc248a5ffa465a05ea83a9d466d4bca0e53ab42771515d4ebb467a41c1)
FetchContent_MakeAvailable(miniz xlsxwriter)
target_compile_definitions(miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# v1.2.4's Windows temp-file helper uses ANSI getenv/stat/_open. A Unicode
# user TEMP then fails whenever its fallback current directory is not writable.
# Keep disk-backed temporary data and replace only this Windows source. The
# fixed source hash makes upgrades fail closed until the adapter is reviewed.
set(xlsxwriter_tmp_source "${xlsxwriter_SOURCE_DIR}/third_party/tmpfileplus/tmpfileplus.c")
file(SHA256 "${xlsxwriter_tmp_source}" xlsxwriter_tmp_sha256)
if(NOT xlsxwriter_tmp_sha256 STREQUAL "bad61b21f48ef26fa82ad95cd01431334774ed528342d9a559de014f14ad7eb7")
  message(FATAL_ERROR "libxlsxwriter tmpfileplus changed; review the Windows Unicode adapter")
endif()
if(WIN32)
  if(USE_STANDARD_TMPFILE OR USE_MEM_FILE OR USE_FMEMOPEN)
    message(FATAL_ERROR "The Windows Excel build requires disk-backed Unicode tmpfileplus")
  endif()
  get_target_property(xlsxwriter_sources xlsxwriter SOURCES)
  set(xlsxwriter_tmp_matches 0)
  set(xlsxwriter_unicode_sources "")
  foreach(source IN LISTS xlsxwriter_sources)
    if(source STREQUAL "third_party/tmpfileplus/tmpfileplus.c" OR source STREQUAL "${xlsxwriter_tmp_source}")
      math(EXPR xlsxwriter_tmp_matches "${xlsxwriter_tmp_matches} + 1")
      list(APPEND xlsxwriter_unicode_sources "${CMAKE_CURRENT_LIST_DIR}/windows_tmpfileplus.c")
    else()
      # The upstream target contains relative vendored source paths. Preserve
      # their source directory when resetting SOURCES from this parent scope.
      if(NOT IS_ABSOLUTE "${source}" AND NOT source MATCHES "^\\$<")
        get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${xlsxwriter_SOURCE_DIR}")
      endif()
      list(APPEND xlsxwriter_unicode_sources "${source}")
    endif()
  endforeach()
  if(NOT xlsxwriter_tmp_matches EQUAL 1)
    message(FATAL_ERROR "Expected exactly one libxlsxwriter tmpfileplus source")
  endif()
  set_property(TARGET xlsxwriter PROPERTY SOURCES "${xlsxwriter_unicode_sources}")
endif()
