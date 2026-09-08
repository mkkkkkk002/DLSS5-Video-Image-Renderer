// Render-parameter metadata embedding for saved media.
//
// The engine stamps every output (mp4 video via ffmpeg -metadata comment, PNG via a tEXt
// chunk, JPG via a COM marker) with a compact JSON of the render-affecting parameters, so a
// user who drags a finished file back into the UI can have the exact settings restored. The
// payload is stored under a fixed key so players/editors ignore it and it never touches pixels.
//
// Container support:
//   mp4/mkv  ffmpeg writes -metadata comment=<payload> (native, handled in video_pipe)
//   png      this file injects a tEXt chunk: keyword "render_cfg", text = payload
//   jpg      this file injects a COM marker (FF FE) right after SOI carrying the payload
//
// Payload format (shared with server.js): "render_cfg=" + base64url(compact JSON). base64url
// keeps the value free of chars that would break ffmpeg's command line or container quoting.
#include "meta_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char kMagic[] = "render_cfg=";

// ---- minimal base64url encoder (payloads are small; clarity over speed) -------------------
const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
std::string b64urlEncode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    const unsigned char* d = (const unsigned char*)in.data();
    while (i + 2 < in.size()) {
        unsigned v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = d[i] << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (d[i] << 16) | (d[i + 1] << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// ---- CRC32 (PNG chunk integrity) -----------------------------------------------------------
uint32_t crc32Table[256];
bool crcTableReady = false;
void initCrcTable() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32Table[n] = c;
    }
    crcTableReady = true;
}
uint32_t crc32Of(const unsigned char* data, size_t len) {
    if (!crcTableReady) initCrcTable();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = crc32Table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool readWhole(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    // Paths may be UTF-8 with non-ASCII (e.g. 中文 folders) — narrow fopen breaks on those.
    std::wstring wp(path.begin(), path.end());
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n > 0) { wp.resize(n - 1); MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], n); }
    if (_wfopen_s(&f, wp.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

bool writeWhole(const std::string& path, const std::vector<unsigned char>& data) {
    FILE* f = nullptr;
    std::wstring wp;
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n > 0) { wp.resize(n - 1); MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], n); }
    if (_wfopen_s(&f, wp.c_str(), L"wb") != 0 || !f) return false;
    size_t nw = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return nw == data.size();
}

// Append one PNG chunk (type + data) with length/CRC framing.
void appendPngChunk(std::vector<unsigned char>& out, const char* type,
                    const unsigned char* data, size_t len) {
    unsigned char lenB[4] = { (unsigned char)(len >> 24), (unsigned char)(len >> 16),
                              (unsigned char)(len >> 8), (unsigned char)len };
    out.insert(out.end(), lenB, lenB + 4);
    size_t typeAt = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + len);
    unsigned char* body = out.data() + typeAt;         // "TYPE" + data (CRC input)
    uint32_t crc = crc32Of(body, 4 + len);
    unsigned char crcB[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                              (unsigned char)(crc >> 8), (unsigned char)crc };
    out.insert(out.end(), crcB, crcB + 4);
}

// tEXt chunk: keyword\0text, keyword limited to 1..79 latin-1, no leading/trailing space.
bool injectPng(const std::string& path, const std::string& payload) {
    std::vector<unsigned char> png;
    if (!readWhole(path, png)) return false;
    // PNG signature + need an IEND chunk to anchor before.
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 8 + 12 || memcmp(png.data(), kSig, 8) != 0) return false;
    // Find the IEND chunk (last chunk) to insert the tEXt chunk right before it.
    size_t pos = 8;
    size_t iend = SIZE_MAX;
    while (pos + 12 <= png.size()) {
        size_t len = ((size_t)png[pos] << 24) | ((size_t)png[pos + 1] << 16) |
                     ((size_t)png[pos + 2] << 8) | png[pos + 3];
        if (pos + 12 + len > png.size()) return false;         // corrupt
        if (memcmp(png.data() + pos + 4, "IEND", 4) == 0) { iend = pos; break; }
        pos += 12 + len;
    }
    if (iend == SIZE_MAX) return false;
    std::string keyword = "render_cfg";
    std::vector<unsigned char> text(keyword.begin(), keyword.end());
    text.push_back(0);
    text.insert(text.end(), payload.begin(), payload.end());
    std::vector<unsigned char> out(png.begin(), png.begin() + iend);
    appendPngChunk(out, "tEXt", text.data(), text.size());
    out.insert(out.end(), png.begin() + iend, png.end());
    return writeWhole(path, out);
}

// JPEG COM segment: FF FE, then 16-bit big-endian length (2 + payload), then payload. Inserted
// right after the SOI marker (FF D8). Max payload 65533 bytes.
bool injectJpg(const std::string& path, const std::string& payload) {
    std::vector<unsigned char> jpg;
    if (!readWhole(path, jpg)) return false;
    if (jpg.size() < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return false;
    if (payload.size() + 2 > 65533) return false;
    std::vector<unsigned char> seg;
    seg.push_back(0xFF);
    seg.push_back(0xFE);
    unsigned len = (unsigned)payload.size() + 2;
    seg.push_back((unsigned char)(len >> 8));
    seg.push_back((unsigned char)(len & 0xFF));
    seg.insert(seg.end(), payload.begin(), payload.end());
    std::vector<unsigned char> out;
    out.reserve(jpg.size() + seg.size());
    out.insert(out.end(), jpg.begin(), jpg.begin() + 2);   // SOI
    out.insert(out.end(), seg.begin(), seg.end());
    out.insert(out.end(), jpg.begin() + 2, jpg.end());
    return writeWhole(path, out);
}

}  // namespace

std::string makeMetaPayload(const std::string& compactJson) {
    return std::string(kMagic) + b64urlEncode(compactJson);
}

bool injectMetaFile(const std::string& pathUtf8, const std::string& payload) {
    // Detect by extension (server only ever hands us png/jpg/jpeg).
    std::string lower = pathUtf8;
    for (auto& c : lower)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0)
        return injectPng(pathUtf8, payload);
    if (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".jpg") == 0 ||
                             lower.compare(lower.size() - 5, 5, ".jpeg") == 0))
        return injectJpg(pathUtf8, payload);
    return false;
}
