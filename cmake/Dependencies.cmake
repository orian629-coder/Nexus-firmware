# Central dependency resolution. Prefer system packages (apt on the Pi, brew on macOS);
# fall back to FetchContent with pinned tags so a bare development host still builds.

include(FetchContent)

find_package(Threads REQUIRED)

# ── nlohmann/json ────────────────────────────────────────────────────────────
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found — fetching v3.11.3")
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# ── spdlog (wraps fmt) ───────────────────────────────────────────────────────
find_package(spdlog 1.9 QUIET)
if(NOT spdlog_FOUND)
  message(STATUS "spdlog not found — fetching v1.13.0")
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0)
  FetchContent_MakeAvailable(spdlog)
endif()

# ── libsodium (crypto) ───────────────────────────────────────────────────────
# No official CMake config; locate via pkg-config first, then a manual search.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
endif()

if(SODIUM_FOUND)
  add_library(nexus::sodium ALIAS PkgConfig::SODIUM)
else()
  find_path(SODIUM_INCLUDE_DIR sodium.h
    PATHS /usr/local/opt/libsodium/include /opt/homebrew/opt/libsodium/include /usr/include)
  find_library(SODIUM_LIBRARY NAMES sodium
    PATHS /usr/local/opt/libsodium/lib /opt/homebrew/opt/libsodium/lib /usr/lib)
  if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
    message(FATAL_ERROR "libsodium not found. Install: brew install libsodium / apt install libsodium-dev")
  endif()
  add_library(nexus_sodium INTERFACE)
  target_include_directories(nexus_sodium INTERFACE ${SODIUM_INCLUDE_DIR})
  target_link_libraries(nexus_sodium INTERFACE ${SODIUM_LIBRARY})
  add_library(nexus::sodium ALIAS nexus_sodium)
endif()

# ── GoogleTest (only when testing) ───────────────────────────────────────────
if(NEXUS_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GTest not found — fetching v1.14.0")
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)
endif()

# ── Real hardware libraries (Pi only, non-stub build) ────────────────────────
# ALSA (audio out + mic), libgpiod (amplifier control), Avahi (mDNS discovery).
if(NOT NEXUS_STUB_HAL)
  if(NOT PkgConfig_FOUND)
    find_package(PkgConfig REQUIRED)
  endif()
  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
  pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod)
  pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
  add_library(nexus::alsa ALIAS PkgConfig::ALSA)
  add_library(nexus::gpiod ALIAS PkgConfig::GPIOD)
  add_library(nexus::avahi ALIAS PkgConfig::AVAHI)
endif()

# ── cpp-httplib (local web server / API) ─────────────────────────────────────
# Needed for the real web transport: the speaker's dashboard on the Pi (non-stub), and the
# streamer's control UI wherever it runs real sockets (e.g. a dev Mac). Header-only.
if(NOT NEXUS_STUB_HAL OR NEXUS_STREAMER_REAL_NET)
  find_path(HTTPLIB_INCLUDE_DIR httplib.h PATHS /usr/include /usr/local/include)
  if(NOT HTTPLIB_INCLUDE_DIR)
    message(STATUS "cpp-httplib not found — fetching v0.15.3")
    FetchContent_Declare(httplib
      GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
      GIT_TAG v0.15.3)
    FetchContent_MakeAvailable(httplib)
  else()
    add_library(httplib INTERFACE)
    target_include_directories(httplib INTERFACE ${HTTPLIB_INCLUDE_DIR})
  endif()
endif()
