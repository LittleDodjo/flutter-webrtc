# Автономная сборка squeak_apm_test для CI плагина (без Flutter):
#   cmake -S common/cpp/test -B build/apm_test [-A x64 | -G Ninja -DCMAKE_BUILD_TYPE=Release]
#   cmake --build build/apm_test --config Release
cmake_minimum_required(VERSION 3.15)
project(squeak_apm_test LANGUAGES CXX C)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  cmake_policy(SET CMP0135 NEW)
endif()

# Скачивает libwebrtc и squeak_df по third_party/libwebrtc_version.ini.
include("${CMAKE_CURRENT_LIST_DIR}/../../../third_party/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/squeak_apm_test.cmake")
squeak_add_apm_test()
