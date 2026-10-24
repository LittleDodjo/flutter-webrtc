# Тест врезки захвата на настоящем APM (common/cpp/test/squeak_apm_test.cc).
# Подключается из windows/ и linux/ CMakeLists плагина — тогда exe собирается
# вместе с `flutter build`, и CI клиента гоняет его на готовом бандле, — и из
# автономного common/cpp/test/CMakeLists.txt для CI плагина.

get_filename_component(SQUEAK_PLUGIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)

set(SQUEAK_RNNOISE_SOURCES
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/celt_lpc.c"
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/denoise.c"
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/kiss_fft.c"
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/pitch.c"
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/rnn.c"
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/rnn_data.c"
  "${SQUEAK_PLUGIN_ROOT}/third_party/rnnoise/src/rnn_reader.c"
)

function(squeak_add_apm_test)
  set(root "${SQUEAK_PLUGIN_ROOT}")
  add_executable(squeak_apm_test
    "${root}/common/cpp/test/squeak_apm_test.cc"
    "${root}/common/cpp/src/squeak_capture_processor.cc"
    "${root}/common/cpp/src/squeak_deep_filter.cc"
    "${root}/common/cpp/src/squeak_resampler.cc"
    ${SQUEAK_RNNOISE_SOURCES}
  )
  target_include_directories(squeak_apm_test PRIVATE
    "${root}/common/cpp/include"
    "${root}/third_party/rnnoise/include"
    "${root}/third_party/rnnoise/src"
    "${root}/third_party/libwebrtc/include"
  )
  target_compile_definitions(squeak_apm_test PRIVATE
    SQUEAK_USE_RNNOISE
    _USE_MATH_DEFINES
  )
  set_target_properties(squeak_apm_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
  )
  if(WIN32)
    target_compile_definitions(squeak_apm_test PRIVATE LIB_WEBRTC_API_DLL)
    target_compile_options(squeak_apm_test PRIVATE /EHsc /utf-8)
    target_link_libraries(squeak_apm_test PRIVATE
      "${root}/third_party/libwebrtc/lib/libwebrtc.dll.lib"
    )
    # Рядом с exe — только движок. squeak_df нарочно не кладём: без него
    # проверяется откат на RNNoise, с ним — путь через SQUEAK_DF_DIR или бандл.
    add_custom_command(TARGET squeak_apm_test POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${root}/third_party/libwebrtc/lib/libwebrtc.dll"
        "$<TARGET_FILE_DIR:squeak_apm_test>/libwebrtc.dll"
    )
  else()
    target_link_libraries(squeak_apm_test PRIVATE
      "${root}/third_party/libwebrtc/lib/libwebrtc.so"
      ${CMAKE_DL_LIBS}
      pthread
    )
    set_target_properties(squeak_apm_test PROPERTIES
      BUILD_RPATH "$ORIGIN;$ORIGIN/lib;${root}/third_party/libwebrtc/lib"
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      # Заголовки libwebrtc без <cstdint> — как у самого плагина.
      target_compile_options(squeak_apm_test PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>")
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      set_source_files_properties(${SQUEAK_RNNOISE_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w")
    endif()
  endif()
endfunction()
