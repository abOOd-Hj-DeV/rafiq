cmake_minimum_required(VERSION 3.16)
project(k230_control LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# K230_TARGET selects the deployment flavour:
#   pc      - development host; FFmpeg software decoder, both cores emulated in one process
#   little  - K230 small core (Linux/buildroot): bridge only, no FFmpeg
#   big     - K230 big core (RT-Smart): inspector only, VDEC + KPU backends
set(K230_TARGET "pc" CACHE STRING "Deployment target: pc | little | big")
set_property(CACHE K230_TARGET PROPERTY STRINGS pc little big)

option(K230_BUILD_TESTS "Build unit tests" ON)
option(K230_ENABLE_ONNX "Enable the PC NSFWJS analyzer using ONNX Runtime" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime C++ SDK prefix (include/ and lib/)")
set(K230_NNCASE_ROOT "" CACHE PATH "Official nncase K230 Linux runtime SDK")
set(K230_GSL_INCLUDE_DIR "" CACHE PATH "gsl-lite include directory")
set(K230_ABI_LIB_DIR "" CACHE PATH "Matching target Linux shared libraries for cross linking")
set(K230_MMZ_ROOT "" CACHE PATH "Official Linux SDK libmmz source directory")

if(K230_NNCASE_ROOT)
  if(NOT K230_TARGET STREQUAL "big" OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Hardware AI2D adapter requires K230 big Linux")
  endif()
  add_library(k230_ai2d SHARED tools/ai2d_runtime.cpp)
  if(NOT EXISTS "${K230_MMZ_ROOT}/mmz.c")
    message(FATAL_ERROR "Hardware AI2D requires the official Linux SDK libmmz source")
  endif()
  enable_language(C)
  target_sources(k230_ai2d PRIVATE "${K230_MMZ_ROOT}/mmz.c")
  set_source_files_properties("${K230_MMZ_ROOT}/mmz.c" PROPERTIES COMPILE_OPTIONS "-march=rv64gc_xtheadcmo;-fvisibility=hidden")
  set_target_properties(k230_ai2d PROPERTIES SKIP_BUILD_RPATH TRUE)
  target_compile_definitions(k230_ai2d PRIVATE BUILDING_RUNTIME)
  target_link_options(k230_ai2d PRIVATE "-Wl,--exclude-libs,ALL")
  if(K230_ABI_LIB_DIR)
    target_link_directories(k230_ai2d BEFORE PRIVATE "${K230_ABI_LIB_DIR}")
    target_link_libraries(k230_ai2d PRIVATE "-Wl,--no-as-needed" c m pthread dl)
  endif()
  target_include_directories(k230_ai2d PRIVATE "${K230_NNCASE_ROOT}/include" "${K230_GSL_INCLUDE_DIR}")
  target_link_libraries(k230_ai2d PRIVATE
    "-Wl,--start-group" "${K230_NNCASE_ROOT}/lib/libfunctional_k230.a"
    "${K230_NNCASE_ROOT}/lib/libNncase.Runtime.Native.a"
    "${K230_NNCASE_ROOT}/lib/libnncase.rt_modules.k230.a" "-Wl,--end-group")
endif()

if(K230_ENABLE_ONNX AND NOT K230_TARGET STREQUAL "pc")
  message(FATAL_ERROR "ONNX Runtime is a PC evaluation backend; K230 uses nncase/KPU")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(common)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(k230-shmem-read tools/shmem_read.cpp)
  add_executable(k230-shmem-probe tools/shmem_probe.cpp)
  target_link_libraries(k230-shmem-probe PRIVATE k230_common)
endif()

if(K230_TARGET STREQUAL "pc" OR K230_TARGET STREQUAL "little")
  add_subdirectory(bridge)
endif()

if(K230_TARGET STREQUAL "pc" OR K230_TARGET STREQUAL "big")
  add_subdirectory(inspector)
endif()

if(K230_TARGET STREQUAL "pc")
  add_subdirectory(apps)
  add_subdirectory(tools)
endif()

if(K230_BUILD_TESTS AND K230_TARGET STREQUAL "pc")
  enable_testing()
  add_subdirectory(tests)
endif()
