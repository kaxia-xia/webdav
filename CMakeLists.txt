cmake_minimum_required(VERSION 3.16)
project(webdav-server
    VERSION 1.0.0
    DESCRIPTION "A lightweight WebDAV server with browser directory listing"
    LANGUAGES CXX
)

# ── Dependencies ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)
find_package(Threads REQUIRED)

# ── Core library ─────────────────────────────────────────────────────────────
add_library(webdav-core STATIC)
target_compile_features(webdav-core PUBLIC cxx_std_20)
target_include_directories(webdav-core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(webdav-core PUBLIC Threads::Threads)

target_sources(webdav-core PRIVATE
    src/utils.cpp          src/utils.h
    src/file_ops.cpp       src/file_ops.h
    src/http_parser.cpp    src/http_parser.h
    src/webdav_handler.cpp src/webdav_handler.h
    src/xml_utils.cpp      src/xml_utils.h
    src/html_dir.cpp       src/html_dir.h
    src/server.cpp         src/server.h
)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(webdav-server)
target_compile_features(webdav-server PUBLIC cxx_std_20)
target_link_libraries(webdav-server PRIVATE webdav-core)
target_sources(webdav-server PRIVATE src/main.cpp)

# ── Compiler warnings ────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(webdav-core   PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(webdav-server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Default install prefix ───────────────────────────────────────────────────
# Use ~/.local when the user doesn't override CMAKE_INSTALL_PREFIX explicitly.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH
        "Install path prefix (default: ~/.local)" FORCE)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
install(TARGETS webdav-server
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
