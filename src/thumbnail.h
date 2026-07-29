#pragma once

#include <string>
#include <string_view>
#include <filesystem>

namespace fs = std::filesystem;

namespace thumbnail {

// Initialize: check if ffmpeg is available, create cache directory
void init();

// Generate a thumbnail for a media file.
// Returns the thumbnail image data (JPEG/PNG) or an SVG icon as fallback.
// If size > 0, thumbnail is scaled to fit within size×size (maintains aspect ratio).
// If size == 0, uses default (256).
std::string generate(const fs::path& filepath, int size = 256);

// Get the MIME type for a generated thumbnail
std::string_view mime_type(const std::string& data);

// Check if a file is a supported media file by extension
bool is_media_file(std::string_view filename);

// Check if a file is a video file (as opposed to audio)
bool is_video_file(std::string_view filename);

// Check if a file is an image file (png, jpg, gif, webp, etc.)
bool is_image_file(std::string_view filename);

} // namespace thumbnail
