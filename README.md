# WebDAV Server

A lightweight, zero-dependency WebDAV server written in C++20.  
Serve files over the WebDAV protocol and browse directories via any web browser.

## Features

- **Full WebDAV support** — GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, MOVE, COPY, LOCK, UNLOCK, OPTIONS
- **HTTP Basic Authentication** — optional username/password via command-line flags
- **Browser-friendly** — auto-detects browser requests and returns a styled HTML directory listing
- **Media grid view** — video/audio files shown as thumbnail cards in a responsive CSS grid
- **Thumbnail generation** — auto-extracts video first-frame / audio cover-art via ffmpeg (cached)
- **Media player** — click any video/audio file to open a full-screen HTML5 player page
- **Advisory file locking** — prevents PUT/DELETE/MOVE on files currently being streamed
- **Zero external dependencies** — only C++20 standard library and POSIX sockets (ffmpeg optional)
- **Path traversal protection** — requests cannot escape the served root directory
- **High concurrency** — epoll-based thread pool with `sendfile()` zero-copy for large files
- **Range requests (HTTP 206)** — seekable streaming for video, audio, and partial downloads
- **Tiny footprint** — ~274 KB binary on ARM64

## Quick Start

```bash
# Serve the current directory on port 9000 (no authentication)
webdav-server

# Serve a specific directory
webdav-server -d /srv/files -p 8080

# With authentication (recommended for public-facing servers)
webdav-server -u alice -w mypassword

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
  -d, --dir <path>      Directory to serve (default: current directory)
  -p, --port <port>     Port to listen on (default: 9000)
  -u, --user <name>     Username for HTTP Basic authentication
  -w, --pass <password> Password for HTTP Basic authentication
  --no-browser          Disable browser-friendly HTML directory listing
  -h, --help            Show this help

Note: --user and --pass must be used together. Both or neither.
```

## Browser Directory Listing

When accessed from a web browser, directories are displayed as a styled HTML page
with two sections:

### 🎬 Media Grid
Video and audio files (`.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`, `.mp3`,
`.flac`, `.ogg`, `.wav`, `.aac`, `.m4a`, `.opus`, `.wma`) are shown as
thumbnail cards in a responsive CSS grid. Each card displays:

- **Thumbnail image** — extracted via ffmpeg (first frame at 2s for video,
  embedded cover art for audio) — or a media-type SVG icon as fallback
- **VIDEO / AUDIO badge** — indicates media type at a glance
- **File name** and **size**

Click any card to open the built-in HTML5 media player.

### 📂 Files & Folders Table
Directories and non-media files are listed in a traditional sortable table
with name, size, and modification date.

## Thumbnails & Media Player

### Automatic Thumbnails

When `ffmpeg` is installed on the system, the server automatically generates
thumbnails for media files:

| File type | Thumbnail source |
|-----------|-----------------|
| Video (`.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`, `.ogv`) | Frame at 2 seconds |
| Audio (`.mp3`, `.flac`, `.ogg`, `.opus`, `.wav`, `.aac`, `.m4a`, `.wma`) | Embedded cover art |

Thumbnails are cached in `/tmp/webdav-thumbnails/` (keyed by file path + mtime)
and served via the internal `/__thumb__` endpoint. Cache entries are generated
once per file and reused until the file is modified.

If ffmpeg is not available, media cards fall back to inline SVG icons
(blue play-button for video, red record for audio).

### HTML5 Media Player

Clicking a media file from the grid opens a dark-themed full-screen player
page with `<video>` or `<audio>` element and a "Back to directory" link.

```
GET /__thumb__?path=<url-encoded-path>&size=<pixels>
```

- `path` — URL-encoded file path relative to served root
- `size` — max dimension in pixels (default: 256, range: 1–1024)
- Returns: JPEG image (with ffmpeg) or SVG icon (fallback)
- Headers: `Cache-Control: public, max-age=3600`

## File Locking

The server uses POSIX advisory locks (`flock`) to prevent conflicts between
concurrent readers and writers:

- **Reading (GET/HEAD)**: A shared lock is held on the file while `sendfile()`
  streams data to the client. The lock is automatically released when the
  transfer completes or the client disconnects.
- **Writing (PUT) / Deleting (DELETE) / Moving (MOVE)**: Before modifying a
  file, the server attempts to acquire an exclusive lock. If the file is
  currently being streamed, the request returns **HTTP 423 Locked**.

This ensures that a video being watched by one user cannot be accidentally
overwritten or deleted by another.

## Authentication

The server supports HTTP Basic Authentication. When `--user` and `--pass` are
provided, every request must include a valid `Authorization` header. Browsers
will show a native login dialog automatically.

```bash
# Start server with auth
webdav-server -u admin -w secret123

# Access from browser → prompts for username and password
http://localhost:9000/

# curl with auth
curl -u admin:secret123 http://localhost:9000/

# WebDAV mount with auth (davfs2)
sudo mount -t davfs http://localhost:9000 /mnt/webdav -o username=admin,password=secret123
```

If no `--user` / `--pass` is given, the server runs without authentication
(open to everyone).

When auth is enabled and a browser accesses a media file, the player page
embeds a short-lived token in the media URL so the browser's `<video>`/`<audio>`
element can stream without re-sending the Authorization header.

## Building from Source

### Prerequisites

- **CMake** ≥ 3.16
- **GCC** ≥ 11 or **Clang** ≥ 14 (C++20 support required)
- **ffmpeg** (optional) — for video/audio thumbnail generation

```bash
# Check toolchain versions
cmake --version
g++ --version

# Optional: install ffmpeg for thumbnails
sudo apt install ffmpeg
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
# Without auth
curl -X PROPFIND -H "Depth: 1" http://localhost:9000/

# With auth
curl -u user:pass -X PROPFIND -H "Depth: 1" http://localhost:9000/

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
    ├── server.h / server.cpp   # TCP server with epoll thread pool + sendfile
    ├── http_parser.h / .cpp    # HTTP request parser & response builder
    ├── webdav_handler.h / .cpp # WebDAV protocol logic + auth + thumbnail endpoint
    ├── file_ops.h / .cpp       # Filesystem operations + advisory locking
    ├── thumbnail.h / .cpp      # ffmpeg thumbnail generation + SVG fallbacks
    ├── xml_utils.h / .cpp      # PROPFIND XML multi-status responses
    ├── html_dir.h / .cpp       # Browser HTML directory listing (grid + table)
    └── utils.h / .cpp          # URL codec, MIME types, base64, time formatting
```

## License

MIT
