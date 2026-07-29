# WebDAV Server

A lightweight, zero-dependency WebDAV server written in C++20.  
Serve files over the WebDAV protocol and browse directories via any web browser.

## Features

- **Full WebDAV support** — GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, MOVE, COPY, LOCK, UNLOCK, OPTIONS
- **Browser-friendly** — auto-detects browser requests and returns a styled HTML directory listing
- **Zero external dependencies** — only C++20 standard library and POSIX sockets
- **Path traversal protection** — requests cannot escape the served root directory
- **Multi-threaded** — handles concurrent clients with `std::jthread`
- **Tiny footprint** — ~160 KB binary on ARM64

## Quick Start

```bash
# Serve the current directory on port 9000
webdav-server

# Serve a specific directory
webdav-server -d /srv/files -p 8080

# Access from a browser
http://localhost:9000/

# Mount as a remote drive (Linux)
sudo mount -t davfs http://localhost:9000 /mnt/webdav

# Mount as a remote drive (macOS Finder)
# Finder → Go → Connect to Server → http://localhost:9000
```

## Usage

```
Usage: webdav-server [OPTIONS]

Options:
  -d, --dir <path>    Directory to serve (default: current directory)
  -p, --port <port>   Port to listen on (default: 9000)
  --no-browser        Disable browser-friendly HTML directory listing
  -h, --help          Show this help
```

## Building from Source

### Prerequisites

- **CMake** ≥ 3.16
- **GCC** ≥ 11 or **Clang** ≥ 14 (C++20 support required)

```bash
# Check toolchain versions
cmake --version
g++ --version
```

### Build & Install

```bash
# Clone the repository
git clone https://github.com/example/webdav-server.git
cd webdav-server

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Install to ~/.local/bin (default)
cmake --install build --prefix ~/.local

# Or install system-wide
sudo cmake --install build --prefix /usr/local
```

### Clean Build

```bash
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## WebDAV Client Examples

### curl

```bash
# List directory properties
curl -X PROPFIND -H "Depth: 1" http://localhost:9000/

# Upload a file
curl -X PUT -d "Hello World" http://localhost:9000/hello.txt

# Download a file
curl http://localhost:9000/hello.txt

# Create a directory
curl -X MKCOL http://localhost:9000/newdir

# Move / rename
curl -X MOVE \
  -H "Destination: http://localhost:9000/renamed.txt" \
  http://localhost:9000/hello.txt

# Delete
curl -X DELETE http://localhost:9000/renamed.txt
```

### cadaver (Linux)

```bash
sudo apt install cadaver
cadaver http://localhost:9000/
```

## Project Structure

```
webdav-server/
├── CMakeLists.txt
├── .gitignore
├── README.md
└── src/
    ├── main.cpp                # Entry point + CLI + signals
    ├── server.h / server.cpp   # TCP server with thread pool
    ├── http_parser.h / .cpp    # HTTP request parser & response builder
    ├── webdav_handler.h / .cpp # WebDAV protocol logic
    ├── file_ops.h / .cpp       # Filesystem operations
    ├── xml_utils.h / .cpp      # PROPFIND XML multi-status responses
    ├── html_dir.h / .cpp       # Browser HTML directory listing
    └── utils.h / .cpp          # URL codec, MIME types, time formatting
```

## License

MIT
