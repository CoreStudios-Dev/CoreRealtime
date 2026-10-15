# Copyright (c) 2026, the CoreRealtime contributors
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

# Identidade unica: CoreRealtime. Target unico: x86_64-unknown-linux-gnu.
project(CoreRealtime LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "[CoreRealtime] Target unico: x86_64-unknown-linux-gnu. Use --toolchain cmake/toolchain-x86_64-linux-gnu.cmake")
endif()

include(cmake/CPM.cmake)

add_subdirectory(Core)

CPMAddPackage("gl:niansa/commoncpp@1.2.1")
CPMAddPackage("gh:Neargye/magic_enum@0.9.7")
CPMAddPackage("gh:boostorg/pfr#boost-1.91.0")

# Nucleo minimo (sem webserver, multiprocessing, settings DB, spdlog, sanmake,
# tracy, FFI, plugins, coroutines, hookpoints)
add_library(corerealtime_lib
    include/luxon/server/apps.hpp  include/luxon/server/global.hpp    include/luxon/server/logger.hpp  src/apps.cpp src/handler_gameserver.cpp    src/lobby.cpp src/platform.cpp
    include/luxon/server/authentication.hpp  include/luxon/server/handler_base.hpp    include/luxon/server/metrics.hpp  src/authentication.cpp src/handler_masterserver.cpp  src/logger.cpp src/platform.hpp
    include/luxon/server/handler_gameserver.hpp    include/luxon/server/peer.hpp  src/handler_nameserver.cpp  src/server_manager.cpp
    include/luxon/server/handler_masterserver.hpp  include/luxon/server/peer_persistence.hpp  src/game.cpp    src/metrics.cpp src/sock_selector.cpp
    include/luxon/server/game.hpp  include/luxon/server/handler_nameserver.hpp    include/luxon/server/server_manager.hpp src/yaml.cpp
    include/luxon/server/sock_selector.hpp  src/peer.cpp src/yaml.hpp
    include/luxon/server/lobby.hpp    include/luxon/server/string_hash.hpp  src/handler_base.cpp src/peer_persistence.cpp
    include/luxon/server/data_model.hpp
    include/luxon/server/pfr_pack.hpp
    include/luxon/server/sqlite3.h src/sqlite3.c
    include/luxon/server/coro_support.hpp
    src/lobby_vt.hpp src/lobby_vt.cpp
)
target_link_libraries(corerealtime_lib
    PRIVATE magic_enum core_http_parser
    PUBLIC commoncpp core_enet core_ser boost_pfr
)
target_include_directories(corerealtime_lib PRIVATE include/luxon/server PUBLIC include)
target_compile_definitions(corerealtime_lib PRIVATE
    SQLITE_CUSTOM_INCLUDE=sqlite3-config.h
    PLATFORM="Linux-x86_64-ubuntu"
    PLATFORM_LINUX
    COREREALTIME_IDENTITY="CoreRealtime"
    COREREALTIME_TARGET_TRIPLE="x86_64-unknown-linux-gnu"
)

add_executable(corerealtime src/main.cpp)
target_link_libraries(corerealtime corerealtime_lib)
set_target_properties(corerealtime PROPERTIES OUTPUT_NAME "corerealtime")
