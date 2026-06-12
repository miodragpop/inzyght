cmake_minimum_required(VERSION 3.15)
project(inzyght)

# ============================================================================
# COMPILER SELECTION
# ============================================================================

# Compiler selection (defaults to system compiler - usually GCC)
# To use Clang, uncomment the lines below:
# set(CMAKE_C_COMPILER clang-20)
# set(CMAKE_CXX_COMPILER clang++-20)

# C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ============================================================================
# VERSION
# ============================================================================
# Read the canonical version from the top-level VERSION file. Bumping the
# version is a one-line edit there — no need to touch CMakeLists.txt or any
# source file. CMake regenerates src/Version.h on every configure.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" INZYGHT_VERSION LIMIT_COUNT 1)
string(STRIP "${INZYGHT_VERSION}" INZYGHT_VERSION)
if(NOT INZYGHT_VERSION)
    message(FATAL_ERROR "VERSION file is empty or missing")
endif()
message(STATUS "Inzyght version: ${INZYGHT_VERSION}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)
# Force CMake to re-run if VERSION changes (so Version.h is regenerated).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# ============================================================================
# FETCHCONTENT - CURL (Static Build)
# ============================================================================
include(FetchContent)

# Re-fetch deps when their GIT_TAG changes in this file. With the default
# (ON), CMake would silently keep the previously-cloned source and ignore
# the new tag — you'd have to `rm -rf build/_deps/<dep>-*` to pick up a
# version bump. With OFF, bumping a GIT_TAG and running ./build.sh is
# enough; FetchContent re-clones and re-builds.
set(FETCHCONTENT_UPDATES_DISCONNECTED OFF)

FetchContent_Declare(CURL
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_18_0  # Latest stable version
    GIT_SHALLOW TRUE
)

# Configure curl for static build
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build curl as static library" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "Build curl executable" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "Enable SSL/TLS support" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL for SSL/TLS" FORCE)
set(CURL_ZLIB ON CACHE BOOL "Enable zlib support" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "Disable Public Suffix List support" FORCE)
set(CURL_USE_ALTSVC OFF CACHE BOOL "Disable alt-svc support" FORCE)
set(CURL_STATICLIB ON CACHE BOOL "Build curl as static library" FORCE)

FetchContent_MakeAvailable(CURL)
message(STATUS "curl 8.18.0 fetched and configured for static build")

# ============================================================================
# FETCHCONTENT - ZEROMQ (Static Build)
# ============================================================================
FetchContent_Declare(ZeroMQ
    GIT_REPOSITORY https://github.com/zeromq/libzmq.git
    GIT_TAG v4.3.5  # Latest stable version
    GIT_SHALLOW TRUE
    PATCH_COMMAND sed -i "s/cmake_minimum_required(VERSION 2.8.12)/cmake_minimum_required(VERSION 3.15...3.28)/" CMakeLists.txt
)

# Configure ZeroMQ for static build
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "Disable ZeroMQ tests" FORCE)
set(ZMQ_BUILD_EXAMPLES OFF CACHE BOOL "Disable ZeroMQ examples" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "Disable ZeroMQ perf tools" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build ZeroMQ as static only" FORCE)
set(ZMQ_BUILD_STATIC ON CACHE BOOL "Build ZeroMQ static library" FORCE)
set(LIBSODIUM_FOUND FALSE CACHE BOOL "Disable libsodium dependency" FORCE)
set(LIBCXX_INCLUDE_DIR "" CACHE STRING "Disable cxx bindings" FORCE)

FetchContent_MakeAvailable(ZeroMQ)
message(STATUS "ZeroMQ 4.3.5 fetched and configured for static build")

# ============================================================================
# FETCHCONTENT - GLAZE (Header-Only Library)
# ============================================================================
FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG v7.7.0  # Latest stable version
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glaze)
message(STATUS "glaze v7.7.0 fetched (header-only)")

# ============================================================================
# FETCHCONTENT - DROGON (Static Build with OpenSSL + PostgreSQL)
# ============================================================================
# We build Drogon from source rather than using the distro package because
# repo-supplied Drogon often lacks OpenSSL, which means no HTTPS without a
# reverse proxy. Building it ourselves with USE_SUBMODULE pulls in the
# bundled trantor and uses our own OpenSSL toolchain.
find_package(OpenSSL REQUIRED)

# OpenSSL must be visible to Drogon's CMake (Trantor probes it).
set(DROGON_BUILD_FROM_SOURCE TRUE)

# Drogon configure-time switches. We turn off everything the indexer doesn't
# need so the resulting static library is as small and fast to build as
# possible. PostgreSQL stays on (Drogon ORM can talk to PG even though our
# own indexer uses libpq directly — keeps the door open).
#
# TRANTOR_USE_TLS=openssl is the critical knob — it pins TLS to OpenSSL and
# causes the build to FAIL if OpenSSL is not found, so a future upgrade of
# Drogon or a missing system OpenSSL surfaces loudly instead of silently
# disabling HTTPS (which is exactly what bit us with the distro package).
set(TRANTOR_USE_TLS        "openssl" CACHE STRING "Pin Trantor TLS provider to OpenSSL" FORCE)
set(BUILD_CTL              OFF CACHE BOOL "Drogon: don't build drogon_ctl"        FORCE)
set(BUILD_EXAMPLES         OFF CACHE BOOL "Drogon: don't build examples"           FORCE)
set(BUILD_DOC              OFF CACHE BOOL "Drogon: don't build docs"               FORCE)
set(BUILD_TESTING          OFF CACHE BOOL "Drogon: don't build tests"              FORCE)
set(BUILD_ORM              ON  CACHE BOOL "Drogon: keep ORM (needed for PG client)" FORCE)
set(BUILD_POSTGRESQL       ON  CACHE BOOL "Drogon: PostgreSQL backend"             FORCE)
set(BUILD_MYSQL            OFF CACHE BOOL "Drogon: no MySQL"                       FORCE)
set(BUILD_SQLITE           OFF CACHE BOOL "Drogon: no SQLite"                      FORCE)
set(BUILD_REDIS            OFF CACHE BOOL "Drogon: no Redis"                       FORCE)
set(BUILD_BROTLI           OFF CACHE BOOL "Drogon: no Brotli"                      FORCE)
set(BUILD_YAML_CONFIG      OFF CACHE BOOL "Drogon: no YAML config (we use JSON)"   FORCE)
set(USE_SUBMODULE          ON  CACHE BOOL "Drogon: build bundled trantor"          FORCE)
set(COZ_PROFILING          OFF CACHE BOOL "Drogon: no coz profiling"               FORCE)
# BUILD_SHARED_LIBS is already OFF globally (set for curl) — Drogon honours it.

FetchContent_Declare(drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG v1.9.13
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE  # need trantor submodule
)

FetchContent_MakeAvailable(drogon)
message(STATUS "Drogon v1.9.13 fetched with OpenSSL + PostgreSQL, static")

# Find PostgreSQL (optional) - try multiple methods
find_package(PostgreSQL QUIET)

if(NOT PostgreSQL_FOUND)
    # Try pkg-config
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PostgreSQL QUIET libpq)
    endif()
endif()

if(NOT PostgreSQL_FOUND)
    # Try pg_config directly
    find_program(PG_CONFIG pg_config)
    if(PG_CONFIG)
        execute_process(
            COMMAND ${PG_CONFIG} --includedir
            OUTPUT_VARIABLE PostgreSQL_INCLUDE_DIRS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND ${PG_CONFIG} --libdir
            OUTPUT_VARIABLE PostgreSQL_LIBRARY_DIRS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        find_library(PostgreSQL_LIBRARIES
            NAMES pq libpq
            PATHS ${PostgreSQL_LIBRARY_DIRS}
            NO_DEFAULT_PATH
        )
        if(PostgreSQL_INCLUDE_DIRS AND PostgreSQL_LIBRARIES)
            set(PostgreSQL_FOUND TRUE)
            message(STATUS "PostgreSQL found via pg_config")
            message(STATUS "  Include: ${PostgreSQL_INCLUDE_DIRS}")
            message(STATUS "  Library: ${PostgreSQL_LIBRARIES}")
        endif()
    endif()
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)  # generated Version.h
if(PostgreSQL_FOUND)
    include_directories(${PostgreSQL_INCLUDE_DIRS})
endif()

# Add glaze headers from FetchContent
include_directories(${glaze_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/main.cc
    src/Logger.cc
    src/ConfigManager.cc
    src/RpcConfig.cc
    src/ycash_rpc_client.cc
    src/controllers/BlockController.cc
    src/controllers/TransactionController.cc
    src/controllers/AddressController.cc
    src/controllers/NetworkController.cc
    src/controllers/SyncController.cc
    src/controllers/WebSocketController.cc
    src/controllers/PageController.cc
    src/services/ZeroMQListener.cc
    src/services/EventBroadcaster.cc
    src/services/BlockchainState.cc
    src/services/RpcResponseCache.cc
    src/services/EventProcessor.cc
    src/services/PollingFallback.cc
    src/services/BlockIndexer.cc
    src/services/GeoLocationService.cc
    src/orm/PostgresCopy.cc
    src/orm/PostgresPool.cc
    src/filters/RateLimiter.cc
)

# Create executable
add_executable(inzyght ${SOURCES})

# Set up main library targets
set(INZYGHT_LIBS Drogon::Drogon OpenSSL::SSL OpenSSL::Crypto)

# Link static curl from FetchContent
list(APPEND INZYGHT_LIBS libcurl)
message(STATUS "Linking static libcurl from FetchContent")

# Add PostgreSQL if found
if(PostgreSQL_FOUND)
    list(APPEND INZYGHT_LIBS ${PostgreSQL_LIBRARIES})
    add_definitions(-DHAVE_PGSQL)
    message(STATUS "PostgreSQL found - building with database indexing support")
else()
    message(WARNING "PostgreSQL not found - building without database indexing (historical blocks will not be indexed)")
endif()

# Add ZeroMQ from FetchContent - force static linking
add_definitions(-DHAVE_ZMQ)
message(STATUS "ZeroMQ 4.3.5 - building with ZeroMQ support (static)")

# Use static library target if available
if(TARGET libzmq-static)
    list(APPEND INZYGHT_LIBS libzmq-static)
    message(STATUS "Linking to libzmq-static target")
elseif(TARGET libzmq)
    # Force static linking by using full path to .a file
    get_target_property(LIBZMQ_LOCATION libzmq IMPORTED_LOCATION)
    if(NOT LIBZMQ_LOCATION)
        get_target_property(LIBZMQ_LOCATION libzmq IMPORTED_LOCATION_RELEASE)
    endif()
    if(LIBZMQ_LOCATION)
        # Replace .so with .a if needed
        string(REPLACE ".so" ".a" LIBZMQ_STATIC "${LIBZMQ_LOCATION}")
        if(EXISTS "${LIBZMQ_STATIC}")
            list(APPEND INZYGHT_LIBS "${LIBZMQ_STATIC}")
            message(STATUS "Linking to static libzmq at ${LIBZMQ_STATIC}")
        else()
            list(APPEND INZYGHT_LIBS libzmq)
            message(STATUS "Static libzmq not found, using dynamic libzmq")
        endif()
    else()
        list(APPEND INZYGHT_LIBS libzmq)
    endif()
else()
    list(APPEND INZYGHT_LIBS libzmq)
    message(STATUS "Linking to libzmq target")
endif()

# Link all libraries to main executable
target_link_libraries(inzyght PUBLIC ${INZYGHT_LIBS})

# Compiler flags for main executable
if(MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(inzyght PRIVATE /W4 /Zi /Od)
    else()
        target_compile_options(inzyght PRIVATE /W4 /O2)
    endif()
else()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(inzyght PRIVATE -Wall -Wextra -Wpedantic -g -O0)
    else()
        target_compile_options(inzyght PRIVATE -Wall -Wextra -Wpedantic -Wno-unused -Wno-maybe-uninitialized -Wno-unused-result -O3)
    endif()
endif()

# Install targets
install(TARGETS inzyght DESTINATION bin)
