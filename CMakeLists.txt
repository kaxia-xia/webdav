cmake_minimum_required(VERSION 3.16)
project(webdav-server VERSION 1.0.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform: need pthread and socket libraries on Linux
if(UNIX)
    set(CMAKE_THREAD_PREFER_PTHREAD ON)
    find_package(Threads REQUIRED)
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/server.cpp
    src/http_parser.cpp
    src/webdav_handler.cpp
    src/file_ops.cpp
    src/xml_utils.cpp
    src/html_dir.cpp
    src/utils.cpp
)

set(HEADERS
    src/server.h
    src/http_parser.h
    src/webdav_handler.h
    src/file_ops.h
    src/xml_utils.h
    src/html_dir.h
    src/utils.h
)

add_executable(webdav-server ${SOURCES} ${HEADERS})

target_include_directories(webdav-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(UNIX)
    target_link_libraries(webdav-server PRIVATE Threads::Threads)
endif()

# Install
include(GNUInstallDirs)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default install prefix" FORCE)
endif()

install(TARGETS webdav-server
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY DESTINATION ${CMAKE_INSTALL_BINDIR}
    DIRECTORY_PERMISSIONS OWNER_WRITE OWNER_READ OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
