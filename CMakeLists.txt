cmake_minimum_required(VERSION 3.20)
project(nexus_speaker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(NEXUS_BUILD_TESTS   "Build unit/integration tests" ON)
option(NEXUS_HW_TESTS      "Build hardware-dependent tests (Pi only)" OFF)
option(NEXUS_STUB_HAL      "Use stub hardware abstraction (dev hosts)" ON)
option(NEXUS_WERROR        "Treat warnings as errors" OFF)
option(NEXUS_BUILD_STREAMER "Build the Nexus Streamer (audio sender + controller)" ON)
# The streamer's networking is plain BSD sockets (works on macOS and Linux), independent of the
# speaker's hardware HAL — so it can send for real from a dev host while the speaker HAL stays
# stubbed. Off only for environments without sockets.
option(NEXUS_STREAMER_REAL_NET "Compile the streamer's real UDP/TCP sockets" ON)

set(NEXUS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(CompilerWarnings)
include(Dependencies)
include(NexusModule)

if(NEXUS_STUB_HAL)
  add_compile_definitions(NEXUS_STUB_HAL=1)
endif()

if(NEXUS_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

if(NEXUS_BUILD_STREAMER)
  add_subdirectory(streamer)
endif()

if(NEXUS_BUILD_TESTS)
  add_subdirectory(tests)
endif()

message(STATUS "nexus-speaker ${PROJECT_VERSION} | tests=${NEXUS_BUILD_TESTS} stub_hal=${NEXUS_STUB_HAL} streamer=${NEXUS_BUILD_STREAMER}")
