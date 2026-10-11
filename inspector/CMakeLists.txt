set(INSPECTOR_SOURCES
  src/pcm_ring.cpp
  src/sync_engine.cpp
  src/analyzer.cpp
  src/policy.cpp
  src/age_policy.cpp
  src/protection_session.cpp
  src/pipeline.cpp
  src/frame_dump.cpp
  src/raw_pcm_decoder.cpp
  src/vdec_decoder.cpp
  src/kpu_analyzer.cpp
  src/kpu_runtime.cpp
  src/nsfwjs_analyzer.cpp
  src/fast_vision.cpp
  src/ui_detector.cpp
)

add_library(k230_inspector STATIC ${INSPECTOR_SOURCES})
target_include_directories(k230_inspector PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(k230_inspector PUBLIC k230_common)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(k230_inspector PRIVATE src/v4l2_decoder.cpp src/nonai2d_converter.cpp)
  target_compile_definitions(k230_inspector PUBLIC K230_HAS_V4L2_API=1)
  if(K230_TARGET STREQUAL "big")
    target_compile_definitions(k230_inspector PRIVATE K230_HAS_V4L2=1)
  endif()
  add_executable(k230-vdec-probe ${PROJECT_SOURCE_DIR}/tools/vdec_probe.cpp)
  target_link_libraries(k230-vdec-probe PRIVATE k230_inspector)
  add_executable(k230-red-demo-detector ${PROJECT_SOURCE_DIR}/tools/red_demo_detector.cpp)
  target_link_libraries(k230-red-demo-detector PRIVATE k230_inspector)
endif()

# Software decoders exist only on the PC; the big core uses the VDEC hardware.
if(K230_TARGET STREQUAL "pc")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  target_sources(k230_inspector PRIVATE src/ffmpeg_decoder.cpp)
  target_link_libraries(k230_inspector PUBLIC PkgConfig::FFMPEG)
  target_compile_definitions(k230_inspector PUBLIC K230_HAS_FFMPEG=1)
endif()

if(K230_ENABLE_ONNX)
  find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}" PATH_SUFFIXES include include/onnxruntime/core/session)
  find_library(ONNXRUNTIME_LIBRARY onnxruntime HINTS "${ONNXRUNTIME_ROOT}" PATH_SUFFIXES lib lib64)
  if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "ONNX Runtime SDK not found. Run python3 tools/setup_onnxruntime.py, then set ONNXRUNTIME_ROOT.")
  endif()
  add_library(onnxruntime SHARED IMPORTED)
  set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")
  target_link_libraries(k230_inspector PUBLIC onnxruntime)
  target_compile_definitions(k230_inspector PUBLIC K230_HAS_ONNX=1)
endif()

# Big-core executable. Reads packets from DATAFIFO (K230) or a .k230rec file
# (PC), decodes, synchronises, analyses and emits verdicts.
add_executable(k230-inspector src/main.cpp)
target_link_libraries(k230-inspector PRIVATE k230_inspector)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND K230_TARGET STREQUAL "big")
  target_compile_definitions(k230-inspector PRIVATE K230_LINUX_PROTECTION_V2=1)
endif()
add_custom_command(TARGET k230-inspector POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${PROJECT_SOURCE_DIR}/tools/kpu_worker.py $<TARGET_FILE_DIR:k230-inspector>/kpu_worker.py
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${PROJECT_SOURCE_DIR}/tools/ai2d_runtime.py $<TARGET_FILE_DIR:k230-inspector>/ai2d_runtime.py
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${PROJECT_SOURCE_DIR}/tools/audio_features.py $<TARGET_FILE_DIR:k230-inspector>/audio_features.py)
if(TARGET k230_ai2d)
  add_dependencies(k230-inspector k230_ai2d)
  add_custom_command(TARGET k230-inspector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:k230_ai2d> $<TARGET_FILE_DIR:k230-inspector>/libk230_ai2d.so)
endif()
