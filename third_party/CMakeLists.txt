include(ExternalProject)

# Flutter adds plugins to example builds through .plugin_symlinks. Windows
# CMake refuses to extract archives through that symlink path, so resolve the
# real third_party directory before downloading or extracting libwebrtc.
get_filename_component(LIBWEBRTC_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)

# Версии, адреса и sha256 бинарей — в libwebrtc_version.ini, чтобы бамп не
# требовал правки CMake. Секция [libwebrtc] — движок, [squeak_df] — шумодав.
set(LIBWEBRTC_VERSION_INI "${LIBWEBRTC_THIRD_PARTY_DIR}/libwebrtc_version.ini")
if(NOT EXISTS "${LIBWEBRTC_VERSION_INI}")
  message(FATAL_ERROR "libwebrtc: missing version manifest at ${LIBWEBRTC_VERSION_INI}")
endif()

set(LIBWEBRTC_BINARY_VERSION "")
set(LIBWEBRTC_DOWNLOAD_URL_BASE "")
set(SQUEAK_DF_BINARY_VERSION "")
set(SQUEAK_DF_DOWNLOAD_URL_BASE "")
set(_ini_section "")
file(STRINGS "${LIBWEBRTC_VERSION_INI}" _libwebrtc_ini_lines)
foreach(_line IN LISTS _libwebrtc_ini_lines)
  string(STRIP "${_line}" _line)
  if(_line STREQUAL "" OR _line MATCHES "^[#;]")
    continue()
  endif()
  if(_line MATCHES "^\\[(.*)\\]$")
    set(_ini_section "${CMAKE_MATCH_1}")
    continue()
  endif()
  if(NOT _line MATCHES "^([^=]+)=(.*)$")
    continue()
  endif()
  string(STRIP "${CMAKE_MATCH_1}" _key)
  string(STRIP "${CMAKE_MATCH_2}" _value)
  if(_ini_section STREQUAL "squeak_df")
    set(_prefix "SQUEAK_DF")
  else()
    set(_prefix "LIBWEBRTC")
  endif()
  if(_key STREQUAL "binary_version")
    set(${_prefix}_BINARY_VERSION "${_value}")
  elseif(_key STREQUAL "download_url")
    set(${_prefix}_DOWNLOAD_URL_BASE "${_value}")
  elseif(_key MATCHES "^sha256_(.+)$")
    string(TOUPPER "${CMAKE_MATCH_1}" _suffix)
    set(${_prefix}_SHA256_${_suffix} "${_value}")
  endif()
endforeach()

if(LIBWEBRTC_BINARY_VERSION STREQUAL "" OR LIBWEBRTC_DOWNLOAD_URL_BASE STREQUAL "")
  message(FATAL_ERROR "libwebrtc: ${LIBWEBRTC_VERSION_INI} must define binary_version and download_url")
endif()

# Strip any trailing slashes so we control the separator when composing the URL.
string(REGEX REPLACE "/+$" "" LIBWEBRTC_DOWNLOAD_URL_BASE "${LIBWEBRTC_DOWNLOAD_URL_BASE}")
string(REGEX REPLACE "/+$" "" SQUEAK_DF_DOWNLOAD_URL_BASE "${SQUEAK_DF_DOWNLOAD_URL_BASE}")

# Resolve OS field for the release asset name (win / linux)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(LIBWEBRTC_OS "win")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LIBWEBRTC_OS "linux")
else()
  message(FATAL_ERROR "libwebrtc: unsupported target OS '${CMAKE_SYSTEM_NAME}'")
endif()

# Resolve arch field for the release asset name (x64 / arm64)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Visual Studio generator exposes the selected arch via CMAKE_GENERATOR_PLATFORM
  string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _libwebrtc_plat)
  if(_libwebrtc_plat STREQUAL "arm64")
    set(LIBWEBRTC_ARCH "arm64")
  elseif(_libwebrtc_plat STREQUAL "x64" OR _libwebrtc_plat STREQUAL "")
    # Empty platform usually means Ninja/default x64 host build
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(LIBWEBRTC_ARCH "x64")
    else()
      message(FATAL_ERROR "libwebrtc: 32-bit Windows is not supported")
    endif()
  else()
    message(FATAL_ERROR "libwebrtc: unsupported Windows arch '${CMAKE_GENERATOR_PLATFORM}'")
  endif()
else()
  # Flutter Linux / eLinux pass the target as e.g. linux-x64 or linux-arm64
  if(DEFINED FLUTTER_TARGET_PLATFORM AND FLUTTER_TARGET_PLATFORM MATCHES "arm64")
    set(LIBWEBRTC_ARCH "arm64")
  elseif(DEFINED FLUTTER_TARGET_PLATFORM AND FLUTTER_TARGET_PLATFORM MATCHES "x64")
    set(LIBWEBRTC_ARCH "x64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(LIBWEBRTC_ARCH "arm64")
  else()
    set(LIBWEBRTC_ARCH "x64")
  endif()
endif()

string(TOUPPER "${LIBWEBRTC_OS}_${LIBWEBRTC_ARCH}" _squeak_variant)

# Скачивание с проверкой sha256 из ini. Без хэша для этой ОС и архитектуры
# сборка останавливается: неизвестный бинарь в неё не попадёт. Архив с
# неверным хэшем выбрасывается и качается заново.
function(_squeak_fetch_verified url zip_path expected_sha256 what)
  if(expected_sha256 STREQUAL "")
    message(FATAL_ERROR "${what}: в libwebrtc_version.ini нет sha256_${LIBWEBRTC_OS}_${LIBWEBRTC_ARCH}")
  endif()
  if(EXISTS "${zip_path}")
    file(SHA256 "${zip_path}" _actual)
    if(_actual STREQUAL "${expected_sha256}")
      return()
    endif()
    message(NOTICE "${what}: хэш ${zip_path} не совпал, качаем заново")
    file(REMOVE "${zip_path}")
  endif()
  message(NOTICE "download: ${url}")
  get_filename_component(_dir "${zip_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")
  file(DOWNLOAD "${url}" "${zip_path}"
      EXPECTED_HASH SHA256=${expected_sha256}
      STATUS _status
      LOG _log)
  list(GET _status 0 _code)
  if(NOT _code EQUAL 0)
    file(REMOVE "${zip_path}")
    message(FATAL_ERROR "${what}: не скачался (${_status}): ${_log}")
  endif()
endfunction()

function(_libwebrtc_extract_and_normalize zip_path dest_dir)
  # Extract into an isolated staging dir so we can normalize the top-level
  # layout regardless of what folder name the archive uses (e.g.
  # libwebrtc-linux-x64-release/ vs. libwebrtc/ vs. files at the root).
  set(_staging "${dest_dir}/.libwebrtc_extract")
  file(REMOVE_RECURSE "${_staging}")
  file(MAKE_DIRECTORY "${_staging}")
  file(ARCHIVE_EXTRACT INPUT "${zip_path}" DESTINATION "${_staging}")

  if(EXISTS "${dest_dir}/libwebrtc")
    file(REMOVE_RECURSE "${dest_dir}/libwebrtc")
  endif()

  file(GLOB _entries "${_staging}/*")
  list(LENGTH _entries _entry_count)
  set(_promoted FALSE)
  if(_entry_count EQUAL 1)
    list(GET _entries 0 _only)
    if(IS_DIRECTORY "${_only}")
      file(RENAME "${_only}" "${dest_dir}/libwebrtc")
      set(_promoted TRUE)
    endif()
  endif()
  if(NOT _promoted)
    # Archive root holds the files directly — wrap them in libwebrtc/.
    file(MAKE_DIRECTORY "${dest_dir}/libwebrtc")
    foreach(_entry IN LISTS _entries)
      get_filename_component(_name "${_entry}" NAME)
      file(RENAME "${_entry}" "${dest_dir}/libwebrtc/${_name}")
    endforeach()
  endif()

  file(REMOVE_RECURSE "${_staging}")
endfunction()

# ---- libwebrtc -------------------------------------------------------------
set(LIBWEBRTC_ASSET "libwebrtc-${LIBWEBRTC_OS}-${LIBWEBRTC_ARCH}-release")
# Версия в имени архива: раньше бамп binary_version на машине с кэшем молча
# оставлял старый движок.
set(ZIPFILE "${LIBWEBRTC_THIRD_PARTY_DIR}/downloads/${LIBWEBRTC_ASSET}-${LIBWEBRTC_BINARY_VERSION}.zip")
set(DOWNLOAD_URL "${LIBWEBRTC_DOWNLOAD_URL_BASE}/${LIBWEBRTC_BINARY_VERSION}/${LIBWEBRTC_ASSET}.zip")
_squeak_fetch_verified("${DOWNLOAD_URL}" "${ZIPFILE}" "${LIBWEBRTC_SHA256_${_squeak_variant}}" "libwebrtc")

# Распаковка по штампу: каталог libwebrtc/ пересобирается, когда версия или
# платформа сменились, иначе остаётся как есть.
set(_libwebrtc_stamp "${LIBWEBRTC_THIRD_PARTY_DIR}/libwebrtc/.version")
set(_libwebrtc_want "${LIBWEBRTC_BINARY_VERSION}-${LIBWEBRTC_OS}-${LIBWEBRTC_ARCH}")
set(_libwebrtc_have "")
if(EXISTS "${_libwebrtc_stamp}")
  file(READ "${_libwebrtc_stamp}" _libwebrtc_have)
  string(STRIP "${_libwebrtc_have}" _libwebrtc_have)
endif()
if(NOT _libwebrtc_have STREQUAL _libwebrtc_want)
  message(NOTICE "libwebrtc: распаковка ${_libwebrtc_want}")
  _libwebrtc_extract_and_normalize("${ZIPFILE}" "${LIBWEBRTC_THIRD_PARTY_DIR}")
  file(WRITE "${_libwebrtc_stamp}" "${_libwebrtc_want}\n")
endif()

# ---- squeak_df (DeepFilterNet3) -------------------------------------------
# Шумодав из third_party/squeak_df, релиз собирает workflow squeak_df.yml.
# Плагин грузит библиотеку в рантайме и без неё откатывается на RNNoise, но
# сборка без неё — ошибка, а не предупреждение: иначе установщик молча уезжает
# без шумодава. SQUEAK_DF_REQUIRED=OFF — выключатель для локальной сборки.
option(SQUEAK_DF_REQUIRED "Сборка падает без DeepFilterNet3" ON)
if(DEFINED ENV{SQUEAK_DF_REQUIRED})
  set(SQUEAK_DF_REQUIRED "$ENV{SQUEAK_DF_REQUIRED}")
endif()
set(SQUEAK_DF_LIBRARY "")
if(SQUEAK_DF_BINARY_VERSION STREQUAL "" OR SQUEAK_DF_DOWNLOAD_URL_BASE STREQUAL "")
  if(SQUEAK_DF_REQUIRED)
    message(FATAL_ERROR "squeak_df: в ${LIBWEBRTC_VERSION_INI} нет секции [squeak_df]")
  endif()
else()
  if(LIBWEBRTC_OS STREQUAL "win")
    set(_squeak_df_file "squeak_df.dll")
  else()
    set(_squeak_df_file "libsqueak_df.so")
  endif()
  set(_squeak_df_asset "squeak_df-${LIBWEBRTC_OS}-${LIBWEBRTC_ARCH}")
  set(_squeak_df_zip "${LIBWEBRTC_THIRD_PARTY_DIR}/downloads/${_squeak_df_asset}-${SQUEAK_DF_BINARY_VERSION}.zip")
  set(_squeak_df_dir "${LIBWEBRTC_THIRD_PARTY_DIR}/squeak_df_bin/${SQUEAK_DF_BINARY_VERSION}")
  set(_squeak_df_url "${SQUEAK_DF_DOWNLOAD_URL_BASE}/${SQUEAK_DF_BINARY_VERSION}/${_squeak_df_asset}.zip")
  set(_squeak_df_sha "${SQUEAK_DF_SHA256_${_squeak_variant}}")
  if(SQUEAK_DF_REQUIRED OR NOT _squeak_df_sha STREQUAL "")
    _squeak_fetch_verified("${_squeak_df_url}" "${_squeak_df_zip}" "${_squeak_df_sha}" "squeak_df")
  endif()
  if(EXISTS "${_squeak_df_zip}" AND NOT EXISTS "${_squeak_df_dir}/${_squeak_df_file}")
    file(MAKE_DIRECTORY "${_squeak_df_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_squeak_df_zip}" DESTINATION "${_squeak_df_dir}")
  endif()
  if(EXISTS "${_squeak_df_dir}/${_squeak_df_file}")
    set(SQUEAK_DF_LIBRARY "${_squeak_df_dir}/${_squeak_df_file}")
  elseif(SQUEAK_DF_REQUIRED)
    message(FATAL_ERROR "squeak_df: ${_squeak_df_file} не найден после распаковки ${_squeak_df_zip}")
  else()
    message(WARNING "squeak_df: библиотеки нет, шумодав останется на RNNoise")
  endif()
endif()
