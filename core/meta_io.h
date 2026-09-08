#pragma once

#include <string>

// Wrap compact JSON in the shared "render_cfg=<base64url>" payload format used by every media
// type (video comment via ffmpeg, PNG tEXt, JPEG COM). Server.js decodes the same format.
std::string makeMetaPayload(const std::string& compactJson);

// Stamp a PNG (tEXt chunk) or JPG (COM marker) file with the payload. Detects type by
// extension; returns false on unrecognised/corrupt input. Safe to call on already-tagged
// files (idempotent in the sense that a second tag is appended but both decode identically).
bool injectMetaFile(const std::string& pathUtf8, const std::string& payload);
