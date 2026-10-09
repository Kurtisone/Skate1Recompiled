# skate1 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(skate1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to the SDK submodule; pass -DREXSDK_DIR=... to use another checkout.
if(NOT REXSDK_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk/CMakeLists.txt")
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk" CACHE PATH "Path to rexglue-sdk source tree")
endif()

include(generated/rexglue.cmake)

# Sources
set(SKATE1_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(skate1 WIN32 ${SKATE1_SOURCES})
else()
    add_executable(skate1 ${SKATE1_SOURCES})
endif()

rexglue_setup_target(skate1)
