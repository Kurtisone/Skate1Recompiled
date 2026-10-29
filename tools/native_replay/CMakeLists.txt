# Offline replay of a native_gpu frame capture (docs/native_d3d.md, milestone B).
#   cmake -S tools/native_replay -B out/native/replay-build -G Ninja
#   cmake --build out/native/replay-build
cmake_minimum_required(VERSION 3.20)
project(native_replay CXX C)
set(CMAKE_CXX_STANDARD 20)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SKATE1_SHADER_CACHE "${ROOT}/out/native/shader_cache.cpp" CACHE FILEPATH "XenosRecomp shader cache")
add_subdirectory(${ROOT}/third_party/plume plume)
find_library(ZSTD_LIBRARY zstd REQUIRED)
add_executable(native_replay replay.cpp ${ROOT}/src/native_gpu/renderer.cpp
    ${SKATE1_SHADER_CACHE} ${ROOT}/third_party/smol-v/smolv.cpp)
target_include_directories(native_replay PRIVATE
    ${ROOT}/src ${ROOT}/src/native_gpu ${ROOT}/third_party/plume ${ROOT}/third_party/smol-v
    ${ROOT}/third_party/rexglue-sdk/thirdparty/xxHash)
target_link_libraries(native_replay PRIVATE plume ${ZSTD_LIBRARY} X11)
target_compile_definitions(native_replay PRIVATE NATIVE_GPU_ZSTD=1)
