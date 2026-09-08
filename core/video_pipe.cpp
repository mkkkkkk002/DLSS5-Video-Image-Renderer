#include "video_pipe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
// windows.h defines min/max macros that collide with std::min/std::max used below.
#undef min
#undef max
// Wide popen: the only reliable way to hand a UTF-8 (Chinese) path to ffmpeg on
// Windows. _popen goes through CreateProcessA and mangles non-ANSI bytes.
#define WPOPEN _wpopen
#define WPCLOSE _pclose
#else
#define WPOPEN popen
#define WPCLOSE pclose
#endif

#include <string>

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], n);
    return ws;
}
std::wstring wquote(const std::wstring& s) { return L"\"" + s + L"\""; }
#else
std::string quote(const std::string& s) { return "\"" + s + "\""; }
#endif

double parseRate(const std::string& s) {
    if (s.empty()) return 0.0;
    size_t slash = s.find('/');
    if (slash == std::string::npos) return atof(s.c_str());
    double num = atof(s.substr(0, slash).c_str());
    double den = atof(s.substr(slash + 1).c_str());
    return den > 0 ? num / den : 0.0;
}

#ifdef _WIN32
std::string runCapture(const std::wstring& wcmd) {
    std::string out;
    FILE* p = WPOPEN(wcmd.c_str(), L"r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    WPCLOSE(p);
    return out;
}
#else
std::string runCapture(const std::string& cmd) {
    std::string out;
    FILE* p = WPOPEN(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    WPCLOSE(p);
    return out;
}
#endif

}  // namespace

bool probeVideo(const std::string& path, VideoInfo& out, std::string& err) {
    err.clear();

#ifdef _WIN32
    std::wstring cmd = L"ffprobe -v error -select_streams v:0 -show_entries "
                       L"stream=width,height,r_frame_rate,duration -of "
                       L"default=noprint_wrappers=1 " +
                       wquote(widen(path));
#else
    std::string cmd = "ffprobe -v error -select_streams v:0 -show_entries "
                      "stream=width,height,r_frame_rate,duration -of default=noprint_wrappers=1 " +
                      quote(path);
#endif
    std::string text = runCapture(cmd);
    if (text.empty()) {
        err = "ffprobe produced no output; is the path valid and is ffprobe on PATH?";
        return false;
    }

    out = VideoInfo{};
    double duration = 0.0;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "width") out.width = atoi(val.c_str());
        else if (key == "height") out.height = atoi(val.c_str());
        else if (key == "r_frame_rate") out.fps = parseRate(val);
        else if (key == "duration") duration = atof(val.c_str());
    }

    if (out.width <= 0 || out.height <= 0) {
        err = "could not read video dimensions";
        return false;
    }

    if (out.fps > 0 && duration > 0) {
        out.nbFrames = (long long)(duration * out.fps + 0.5);
    }

#ifdef _WIN32
    std::wstring audioCmd = L"ffprobe -v error -select_streams a -show_entries stream=index "
                            L"-of csv=p=0 " +
                            wquote(widen(path));
#else
    std::string audioCmd = "ffprobe -v error -select_streams a -show_entries stream=index "
                           "-of csv=p=0 " +
                           quote(path);
#endif
    std::string audio = runCapture(audioCmd);
    out.hasAudio = !audio.empty();

    return true;
}

// ------------------------------------------------------------------ reader

bool VideoReader::open(const std::string& path, int width, int height, double fps,
                       double startSec, double durationSec, bool preferHw) {
    // Decode at source resolution: raw RGBA frame size follows width/height.
    const int outW = width;
    const int outH = height;

    m_hw = false;

#ifdef _WIN32
    std::wstring wpath = widen(path);
    std::wstring cmd;
    cmd = L"ffmpeg -v error";
    if (preferHw) {
        cmd += L" -hwaccel cuda";   // NVDEC: leaves entropy decode on the GPU
        m_hw = true;
    }
    cmd += L" -i " + wquote(wpath) +
          L" -ss " + std::to_wstring(startSec) +
          L" -an -f rawvideo -pix_fmt rgba -s " +
          std::to_wstring(outW) + L"x" + std::to_wstring(outH);
    if (fps > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L" -r %.6f", fps);
        cmd += buf;
    }
    if (durationSec > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L" -t %.6f", durationSec);
        cmd += buf;
    }
    cmd += L" -";
    m_pipe = WPOPEN(cmd.c_str(), L"rb");
#else
    std::string hw;
    if (preferHw) {
        hw = " -hwaccel cuda";
        m_hw = true;
    }
    char cmd[4096];
    char timeBuf[128];
    if (durationSec > 0)
        snprintf(timeBuf, sizeof(timeBuf), " -ss %.6f -t %.6f", startSec, durationSec);
    else
        snprintf(timeBuf, sizeof(timeBuf), " -ss %.6f", startSec);
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error%s -i %s%s -an -f rawvideo -pix_fmt rgba -s %dx%d -r %.6f -",
             hw.c_str(), quote(path).c_str(), timeBuf, outW, outH, fps);
    m_pipe = WPOPEN(cmd, "rb");
#endif
    return m_pipe != nullptr;
}

bool VideoReader::readFrame(uint8_t* dst, size_t bytes) {
    if (!m_pipe) return false;
    return fread(dst, 1, bytes, m_pipe) == bytes;
}

void VideoReader::close() {
    if (m_pipe) {
        // _pclose flushes the pipe, waits for the child and returns its exit status
        // (0 = clean completion). A decoder that dies mid-stream therefore shows up as
        // a non-zero status here, which lets the caller tell a real NVDEC failure apart
        // from a normal end-of-window EOF.
        m_exitStatus = WPCLOSE(m_pipe);
        m_pipe = nullptr;
    }
}

// ------------------------------------------------------------------ writer

bool VideoWriter::open(const std::string& outPath, int width, int height, double fps,
                       const std::string& encoder, const std::string& audioSrc,
                       double audioStartSec, const std::string& extraArgs,
                       const std::string& pixFmt, bool raw16, const std::string& metaComment) {
    std::string encArgs;
    if (encoder == "h264_nvenc") {
        encArgs = "-c:v h264_nvenc -preset p4 -rc vbr -cq 20 -b:v 0";
    } else if (encoder == "hevc_nvenc") {
        encArgs = "-c:v hevc_nvenc -preset p4 -rc vbr -cq 22 -b:v 0";
    } else if (encoder == "libx264") {
        encArgs = "-c:v libx264 -crf 18 -preset medium";
    } else if (encoder == "libx265") {
        encArgs = "-c:v libx265 -crf 20 -preset medium";
    } else {
        encArgs = "-c:v " + encoder;
    }
    if (!extraArgs.empty()) encArgs += " " + extraArgs;

    // Render-parameter metadata: written as a container-level comment (payload is base64url,
    // shell-safe, no quoting issues). mp4/mkv both accept -metadata comment.
    std::string metaArg;
    if (!metaComment.empty()) metaArg = " -metadata comment=\"" + metaComment + "\"";

    // When the decode window starts past zero the audio stream must be trimmed to the same
    // offset, otherwise the soundtrack drifts out of sync with the cropped video.
    std::string audioTrim;
    if (!audioSrc.empty() && audioStartSec > 0) {
        audioTrim = " -ss " + std::to_string(audioStartSec);
    }

#ifdef _WIN32
    // raw16: feed 16-bit-per-channel RGBA (rgba64le) so 10/12-bit HEVC output keeps the full
    // depth instead of passing through an 8-bit quantisation.
    std::wstring cmd = L"ffmpeg -y -v error -f rawvideo -pix_fmt " +
                       std::wstring(raw16 ? L"rgba64le" : L"rgba") + L" -s " +
                       std::to_wstring(width) + L"x" + std::to_wstring(height);
    // NVENC needs its native semi-planar 10-bit input (p010le): swscale cannot feed
    // rgba64le -> yuv420p10le to the hardware encoder directly, so convert with an explicit
    // format filter. Software x265 takes the planar yuv420p10le path.
    std::string pixFilter;
    std::string outFmt = pixFmt;
    if (raw16 && encoder.find("nvenc") != std::string::npos) {
        pixFilter = " -vf format=p010le";
        outFmt = "p010le";
    }
    wchar_t buf[64];
    swprintf(buf, 64, L" -r %.6f", fps);
    cmd += buf;
    cmd += L" -i -";
    if (!audioSrc.empty()) {
        cmd += widen(audioTrim);
        cmd += L" -i " + wquote(widen(audioSrc));
        cmd += L" -map 0:v:0 -map 1:a:0 -c:a copy -shortest";
    } else {
        cmd += L" -map 0:v:0";
    }
    cmd += L" " + widen(encArgs) + widen(pixFilter) + L" -pix_fmt " + widen(outFmt) +
           widen(metaArg) + L" -movflags +faststart " + wquote(widen(outPath));
    m_pipe = WPOPEN(cmd.c_str(), L"wb");
#else
    std::string cmd = "ffmpeg -y -v error -f rawvideo -pix_fmt rgba -s " +
                      std::to_string(width) + "x" + std::to_string(height);
    cmd += " -r " + std::to_string(fps);
    cmd += " -i -";
    if (!audioSrc.empty()) {
        cmd += audioTrim;
        cmd += " -i " + quote(audioSrc);
        cmd += " -map 0:v:0 -map 1:a:0 -c:a copy -shortest";
    } else {
        cmd += " -map 0:v:0";
    }
    cmd += " " + encArgs + " -pix_fmt yuv420p" + metaArg + " -movflags +faststart " +
           quote(outPath);
    m_pipe = WPOPEN(cmd, "wb");
#endif
    return m_pipe != nullptr;
}

bool VideoWriter::writeFrame(const uint8_t* src, size_t bytes) {
    if (!m_pipe) return false;
    return fwrite(src, 1, bytes, m_pipe) == bytes;
}

void VideoWriter::close() {
    if (m_pipe) {
        fflush(m_pipe);
        WPCLOSE(m_pipe);
        m_pipe = nullptr;
    }
}
