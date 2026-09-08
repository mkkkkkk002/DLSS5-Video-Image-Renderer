#pragma once

#include <cstdint>
#include <string>

// Raw RGBA frames in and out, with ffmpeg doing the container work.
//
// ffmpeg is driven as a child process over a pipe rather than linked as a library: it keeps the
// build to a single command with no external SDK, and for an offline pass the pipe is not the
// bottleneck -- the model is.

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    long long nbFrames = 0;
    bool hasAudio = false;
};

// Reads width, height, frame rate, frame count and whether there is an audio stream.
bool probeVideo(const std::string& path, VideoInfo& out, std::string& err);

class VideoReader {
public:
    VideoReader() = default;
    ~VideoReader() { close(); }

    // Decodes to packed RGBA at the source resolution. fps of 0 keeps the source timing.
    // startSec/durationSec crop the decode window: startSec seeks to that time, durationSec>0
    // stops the decoder after that many seconds (so it exits on its own, avoiding a pipe
    // deadlock when the caller stops reading early). The raw frames are width by height.
    // preferHw requests NVDEC (-hwaccel cuda) so entropy decoding runs on the GPU; the decode
    // still lands back in system memory and gets software-scaled to RGBA. The caller detects a
    // failed hardware launch by reading the first frame and reopens with preferHw=false.
    bool open(const std::string& path, int width, int height, double fps = 0.0,
              double startSec = 0.0, double durationSec = 0.0, bool preferHw = false);
    bool readFrame(uint8_t* dst, size_t bytes);
    // Closes the child process and stores its exit status. Call before exitStatus().
    void close();
    // Exit status of the ffmpeg child captured by the last close(): 0 means ffmpeg ended on
    // its own after producing the full decode window (clean EOF); non-zero means the decoder
    // died early (e.g. an NVDEC failure). -1 means no pipe was open / close failed. Only valid
    // after close().
    int exitStatus() const { return m_exitStatus; }
    bool isOpen() const { return m_pipe != nullptr; }
    bool usedHw() const { return m_hw; }

private:
    FILE* m_pipe = nullptr;
    bool m_hw = false;
    int m_exitStatus = -1;
};

class VideoWriter {
public:
    VideoWriter() = default;
    ~VideoWriter() { close(); }

    // Encodes packed RGBA from stdin. audioSrc, when non-empty, is mapped in with -c:a copy so
    // the original soundtrack survives untouched. audioStartSec, when non-zero, trims the audio
    // source to start at that offset so it stays in sync with a cropped decode window.
    // metaComment, when non-empty, is written as a container-level comment metadata tag (the
    // render-parameter payload shared with server.js); it must be shell-safe (no quotes) since
    // it is spliced into the ffmpeg command line.
    bool open(const std::string& outPath, int width, int height, double fps,
              const std::string& encoder, const std::string& audioSrc,
              double audioStartSec = 0.0, const std::string& extraArgs = "",
              const std::string& pixFmt = "yuv420p", bool raw16 = false,
              const std::string& metaComment = "");
    bool writeFrame(const uint8_t* src, size_t bytes);
    void close();
    bool isOpen() const { return m_pipe != nullptr; }

private:
    FILE* m_pipe = nullptr;
};
