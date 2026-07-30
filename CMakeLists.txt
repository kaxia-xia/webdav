cmake_minimum_required(VERSION 3.16)
project(webdav-server
    VERSION 1.2.0
    DESCRIPTION "A lightweight WebDAV server with browser directory listing"
    LANGUAGES CXX C
)

# ── Dependencies ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)
find_package(Threads REQUIRED)

# liburing — try pkg-config first, then manual paths
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBURING QUIET liburing)
endif()

if(LIBURING_FOUND)
    message(STATUS "Found liburing via pkg-config: ${LIBURING_LIBRARIES}")
else()
    # Manual search (built from source in ~/.local or system paths)
    find_path(LIBURING_INCLUDE_DIR liburing.h
        HINTS /home/kaxia/.local/include
              /usr/local/include
              /usr/include
    )
    find_library(LIBURING_LIBRARY
        NAMES uring
        HINTS /home/kaxia/.local/lib
              /usr/local/lib
              /usr/lib/aarch64-linux-gnu
              /usr/lib
        PATH_SUFFIXES aarch64-linux-gnu
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        set(LIBURING_FOUND TRUE)
        set(LIBURING_INCLUDE_DIRS ${LIBURING_INCLUDE_DIR})
        set(LIBURING_LIBRARIES ${LIBURING_LIBRARY})
        message(STATUS "Found liburing: ${LIBURING_LIBRARY}")
    else()
        message(FATAL_ERROR "liburing not found. Install liburing-dev or build from source.")
    endif()
endif()

# ── Core library ─────────────────────────────────────────────────────────────
add_library(webdav-core STATIC)
target_compile_features(webdav-core PUBLIC cxx_std_20)
target_include_directories(webdav-core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    ${LIBURING_INCLUDE_DIRS}
)
target_link_libraries(webdav-core PUBLIC Threads::Threads ${LIBURING_LIBRARIES})

target_sources(webdav-core PRIVATE
    src/utils.cpp          src/utils.h
    src/file_ops.cpp       src/file_ops.h
    src/http_parser.cpp    src/http_parser.h
    src/webdav_handler.cpp src/webdav_handler.h
    src/xml_utils.cpp      src/xml_utils.h
    src/html_dir.cpp       src/html_dir.h
    src/thumbnail.cpp      src/thumbnail.h
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
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH
        "Install path prefix (default: ~/.local)" FORCE)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
install(TARGETS webdav-server
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
