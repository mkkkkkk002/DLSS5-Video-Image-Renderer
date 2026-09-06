// dlss5nr - DLSS 5 Neural Rendering over a video file.
//
// The model is a post-process, not an upscaler: it takes a finished frame at its output
// resolution and synthesises detail. Input and output resolutions are therefore identical.
// A real game feeds depth and per-pixel motion vectors; offline we have neither, so motion comes
// from NVIDIA hardware optical flow (NV-OF) computed on the decoded frames (--frame-guidance 3),
// or the motion texture stays all zeros when NV-OF is switched off (Force Zero). Depth is
// optional: DeepAnything real depth or a Sobel CPU proxy via --depth-interval.

#include <windows.h>
// Windows.h defines min/max macros that clash with std::min/std::max used below.
#undef min
#undef max

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "d3d12_ctx.h"
#include "dlssnr.h"
#include "ngx_params.h"
#include "video_pipe.h"
#include "nvof_flow.h"
#include "depth_anything.h"
#include "blend_pass.h"
#include "densify_pass.h"

namespace {

std::string narrow(const wchar_t* ws) {    if (!ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], n);
    return ws;
}

// CLI fallback for the model dll: the default template name ("nvngx_dlssnr.dll" /
// "nvngx.dll_dlssnr.dll") may not exist once builds carry explicit _fp16/_fp8 suffixes, so probe
// the suffixed variants when the template is missing. Only applied to the default names; an
// explicit --snippet/--forwarder is used as-is (server always passes explicit absolute paths).
// Since Sep-2026 the model dlls live in <root>/models/, probe both the cwd-relative and the
// models/-relative locations (plus ../models/ for bare CLI runs started inside core/).
static std::string probeModelDll(const std::string& name) {
    if (name.empty()) return name;
    auto probe = [&](const std::string& base) -> std::string {
        for (const auto& l : { base, "models/" + base, "../models/" + base }) {
            if (std::filesystem::exists(widen(l))) return l;
        }
        return std::string();
    };
    std::string hit;
    if (!(hit = probe(name)).empty()) return hit;
    if (name.size() > 4) {
        const std::string stem = name.substr(0, name.size() - 4);
        for (const char* suf : { "_fp16", "_fp8" }) {
            if (!(hit = probe(stem + suf + ".dll")).empty()) return hit;
        }
    }
    return name;  // keep the template; the load will fail with a clear error
}

// Sobel-based depth proxy: edge/gradient magnitude per pixel. Not a real depth (no game-engine
// depth available offline) but gives the model geometric context to stabilise reprojection.
void estimateDepthFromColor(const uint8_t* rgba, int W, int H, float* depth) {
    auto L = [&](int px, int py) -> float {
        if (px < 0 || py < 0 || px >= W || py >= H) return 0.0f;
        const uint8_t* p = rgba + ((size_t)py * W + px) * 4;
        return (p[0] * 77 + p[1] * 150 + p[2] * 29) / 256.0f;
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float gx = -L(x - 1, y - 1) - 2 * L(x - 1, y) - L(x - 1, y + 1) + L(x + 1, y - 1) +
                       2 * L(x + 1, y) + L(x + 1, y + 1);
            float gy = -L(x - 1, y - 1) - 2 * L(x, y - 1) - L(x + 1, y - 1) + L(x - 1, y + 1) +
                       2 * L(x, y + 1) + L(x + 1, y + 1);
            float mag = sqrtf(gx * gx + gy * gy) / 1020.0f;  // normalise Sobel to ~[0,1]
            depth[(size_t)y * W + x] = mag;
        }
    }
}

// Per-job depth cache. interval=N means recompute from color every N frames and reuse in
// between; interval=0 means leave the texture as zero (Force Zero).
struct DepthBuffer {
    std::vector<float> depth;
    bool initialized = false;
    int frameCount = 0;
};

void updateDepth(const uint8_t* cur, int W, int H, int interval, DepthBuffer& state) {
    if ((int)state.depth.size() != W * H) {
        state.depth.assign((size_t)W * H, 0.0f);
        state.initialized = false;
        state.frameCount = 0;
    }
    bool shouldUpdate = !state.initialized || (interval > 0 && state.frameCount % interval == 0);
    if (shouldUpdate) {
        estimateDepthFromColor(cur, W, H, state.depth.data());
        state.initialized = true;
    }
    state.frameCount++;
}
// <stem>_nr<ext>, then <stem>_nr_1<ext>, _2, ... until the name is free.
std::string makeUniqueOutput(const std::string& inputUtf8) {
    namespace fs = std::filesystem;
    fs::path p(widen(inputUtf8));
    fs::path dir = p.parent_path();
    if (dir.empty()) dir = fs::path(L".");
    std::wstring stem = p.stem().wstring();
    std::wstring ext = p.extension().wstring();
    if (ext.empty()) ext = L".mp4";
    int n = 0;
    std::wstring cand;
    do {
        std::wstring suffix = (n == 0) ? L"_nr" : (L"_nr_" + std::to_wstring(n));
        cand = (dir / (stem + suffix + ext)).wstring();
        n++;
    } while (fs::exists(cand));
    return narrow(cand.c_str());
}

// Half-float (R16G16B16A16_FLOAT readback) -> float. Values we read are in 0..1.
static inline float halfToFloat(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t bits;
    if (e == 0) {
        if (m == 0) bits = s;
        else {
            int ex = 127 - 24;   // subnormal half: value = m * 2^-24
            while ((m & 0x400u) == 0) { m <<= 1; --ex; }
            m &= 0x3ffu;
            bits = s | ((uint32_t)ex << 23) | (m << 13);
        }
    } else if (e == 31) {
        bits = s | 0x7f800000u;  // inf/nan -> clamps to 1 downstream
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Blend the 16F NR result against the original (residualMult > 1) and quantise to 8-bit
// exactly once, using a 4x4 Bayer ordered dither. The NR output used to be stored straight
// into an R8G8B8A8_UNORM UAV (a silent round per pixel); the denoiser smoothed the grain
// that used to hide 8-bit quantisation, so plain rounding left hard posterisation bands.
static void finalizeToRgba8(const uint8_t* inRGBA, const uint8_t* outF16, uint8_t* dst,
                            uint32_t width, uint32_t height, float residualMult) {
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6},
                                        {3, 11, 1, 9}, {15, 7, 13, 5}};
    float m = residualMult;
    if (m < 0.f) m = 0.f;
    if (m > 2.f) m = 2.f;
    const float inW = 1.f - m;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* bayRow = bayer[y & 3];
        for (uint32_t x = 0; x < width; ++x) {
            const size_t px = (size_t)y * width + x;
            const uint8_t* in = inRGBA + px * 4;
            const uint16_t* h = (const uint16_t*)(outF16 + px * 8);
            float r = halfToFloat(h[0]);
            float g = halfToFloat(h[1]);
            float b = halfToFloat(h[2]);
            if (inW != 0.f) {
                const float inv = 1.f / 255.f;
                r = inW * (float)in[0] * inv + m * r;
                g = inW * (float)in[1] * inv + m * g;
                b = inW * (float)in[2] * inv + m * b;
            }
            if (r < 0.f) r = 0.f; else if (r > 1.f) r = 1.f;
            if (g < 0.f) g = 0.f; else if (g > 1.f) g = 1.f;
            if (b < 0.f) b = 0.f; else if (b > 1.f) b = 1.f;
            const float d = ((float)bayRow[x & 3] + 0.5f) / 16.f - 0.5f;
            uint8_t* o = dst + px * 4;
            o[0] = (uint8_t)(int)(r * 255.f + d + 0.5f);
            o[1] = (uint8_t)(int)(g * 255.f + d + 0.5f);
            o[2] = (uint8_t)(int)(b * 255.f + d + 0.5f);
            o[3] = 255;
        }
    }
}

// 16-bit final image: residual-blend the 16F NR result with the 8-bit input in float, then
// scale to 0..65535 WITHOUT dither (65536 levels make banding physically impossible). Used by
// --png16 for the first rendered frame (image path): no 8-bit quantisation ever happens.
static void finalize16(const uint8_t* inRGBA, const uint8_t* outF16, uint16_t* dst, uint32_t width,
                       uint32_t height, float residualMult) {
    float m = residualMult;
    if (m < 0.f) m = 0.f;
    if (m > 2.f) m = 2.f;
    const float inW = 1.f - m;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* in = inRGBA + (size_t)y * width * 4;
        const uint16_t* h = (const uint16_t*)(outF16 + (size_t)y * width * 8);
        uint16_t* o = dst + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; ++x, in += 4, h += 4, o += 4) {
            float r = halfToFloat(h[0]), g = halfToFloat(h[1]), b = halfToFloat(h[2]);
            if (inW != 0.f) {
                const float inv = 1.f / 255.f;
                r = inW * (float)in[0] * inv + m * r;
                g = inW * (float)in[1] * inv + m * g;
                b = inW * (float)in[2] * inv + m * b;
            }
            if (r < 0.f) r = 0.f; else if (r > 1.f) r = 1.f;
            if (g < 0.f) g = 0.f; else if (g > 1.f) g = 1.f;
            if (b < 0.f) b = 0.f; else if (b > 1.f) b = 1.f;
            o[0] = (uint16_t)(int)(r * 65535.f + 0.5f);
            o[1] = (uint16_t)(int)(g * 65535.f + 0.5f);
            o[2] = (uint16_t)(int)(b * 65535.f + 0.5f);
            o[3] = 65535;
        }
    }
}

// Writes a 16-bit RGB PNG through ffmpeg (raw rgba64le -> rgb48be PNG), encoding one frame.
static bool writePng16(const std::string& pathUtf8, uint32_t width, uint32_t height,
                       const uint16_t* rgba16) {
    std::wstring cmd = L"ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba64le -s " +
                       std::to_wstring(width) + L"x" + std::to_wstring(height) +
                       L" -i - -frames:v 1 -pix_fmt rgb48be \"" + widen(pathUtf8) + L"\"";
    FILE* p = _wpopen(cmd.c_str(), L"wb");
    if (!p) return false;
    const size_t row = (size_t)width * 4 * 2;
    const char* src = (const char*)rgba16;
    for (uint32_t y = 0; y < height; ++y) {
        if (fwrite(src + (size_t)y * row, 1, row, p) != row) {
            _pclose(p);
            return false;
        }
    }
    return _pclose(p) == 0;
}

struct Options {
    std::string input;
    std::string output;
    std::string snippet = "nvngx_dlssnr.dll";
    std::string forwarder = "nvngx.dll_dlssnr.dll";
    std::string encoder = "h264_nvenc";
    std::string extraArgs;
    std::string pixFmt = "yuv420p";  // output pixel format; yuv444p (+lossless encoder) for preview
    std::string dumpFrame;       // optional: write raw first decoded frame as PPM (preview)
    double startTime = 0.0;      // decode window start, seconds
    double endTime = 0.0;        // decode window end, seconds; 0 = end of file
    bool keepAudio = true;
    bool daemon = false;         // resident mode: keep loaded resources, serve jobs from stdin
    int frameGuidance = 3;     // 0 = Force Zero (no motion), 3 = NV-OF hardware optical flow
    int depthInterval = 0;     // Update depth-from-color every N frames; 0 = Force Zero
    bool hwDecode = false;     // --hw-decode: NVDEC. Measured no faster than software decode in
                               // this pipeline (data still round-trips to system memory) and it
                               // has shown mid-stream stalls, so software is the default.
    float residualMult = 1.0f; // 1.0 = pure model output; >1 amplifies detail residual like Magpie
    bool frameReset = false;   // Per-frame reset: treat every frame independently (like a
                               // real-time filter over a video window, no cross-frame history)
    bool perf = false;         // --perf: print per-stage ms/frame breakdown at the end
    bool bypassNr = false;     // --bypass-nr: skip DLSS NR inference (diagnostic passthrough)
    std::string png16;         // --png16 <path>: write first frame as a 16-bit PNG (no banding)
    DlssNrSettings nr;
};

void usage() {
    printf(
        "dlss5nr - run DLSS 5 Neural Rendering over a video\n\n"
        "  dlss5nr --input in.mp4 --output out.mp4 [options]\n\n"
        "  --snippet <path>     nvngx_dlssnr.dll        (default: nvngx_dlssnr.dll)\n"
        "  --forwarder <path>   nvngx.dll_dlssnr.dll    (default: nvngx.dll_dlssnr.dll)\n"
        "  --encoder <name>     h264_nvenc | hevc_nvenc | libx264 | libx265\n"
        "  --start-time <s>     process from this time offset (seconds)\n"
        "  --end-time <s>       process up to this time (seconds); 0 = to the end\n"
        "  --dump-frame <p.ppm> save the raw (unmodified) first decoded frame as a PPM image,\n"
        "                       used by the single-frame preview so the UI gets an exact\n"
        "                       before/after pair without decoding the video twice\n"
        "  --no-audio           drop the source audio instead of copying it\n"
        "  --hw-decode          NVDEC decode (experimental; not faster here, can stall)\n"
        "  --codec-args <s>     extra arguments appended to the encoder\n"
        "  --pix-fmt <s>         output pixel format (default yuv420p; yuv444p keeps 4:4:4 chroma)\n"
        "  --frame-reset        process every frame independently (no cross-frame history),\n"
        "                       matching how a real-time filter over a video behaves\n"
        "  --bypass-nr          skip the DLSS NR inference (diagnostic: input -> colour/dither\n"
        "                       path only, lets you isolate model artifacts from banding/grid)\n"
        "  --png16 <png>        write the first rendered frame as a 16-bit RGB PNG (65536 levels,\n"
        "                       no 8-bit quantisation => no colour banding; used by the image path)\n\n"
        "  Model controls (latched at feature creation):\n"
        "  --preset <0..3>            NR Preset\n"
        "  --intensity <f>            NR Intensity\n"
        "  --style <0..2>             NR Style: 0 default, 1 natural, 2 cinematic\n"
        "  --local-tone <f>           Local Tone Strength\n"
        "  --local-structure <f>      Local Structure Strength\n"
        "  --skin-structure <f>       Skin Structure Strength\n"
        "  --auto-mask <0|1>          Automatic Mask\n"
        "  --ui-correction <0|1>      NR UI Correction\n\n"
        "  Temporal guides:\n"
        "  --frame-guidance <0|3>    motion source: 0 Force Zero (no motion),\n"
        "                            3 NVIDIA hardware optical flow (NV-OF).\n"
        "                            NV-OF needs a supported NVIDIA GPU + driver; if it is\n"
        "                            requested but unavailable the job fails with an error\n"
        "                            rather than silently degrading\n"
        "  --depth-interval <N>       update depth from DepthAnything every N frames\n"
        "                            (0 = Force Zero depth)\n"
        "  --residual-mult <f>        residual reconstruction: out = in + (model-in)*f\n"
        "                              (1.0..2.0; 1.0 = pure model output, Magpie defaults ~1.1)\n\n"
        "  --perf                     print per-stage ms/frame breakdown (decode/NV-OF/depth/\n"
        "                              upload/evaluate/download/encode) at the end of the run\n\n"
        "Progress is written to stdout as: PROGRESS <done>/<total>\n");
}

bool parseInt(const char* s, int& out) {
    return s && sscanf(s, "%d", &out) == 1;
}
bool parseFloat(const char* s, float& out) {
    return s && sscanf(s, "%f", &out) == 1;
}
bool parseLong(const char* s, long long& out) {
    return s && sscanf(s, "%lld", &out) == 1;
}
bool parseDouble(const char* s, double& out) {
    return s && sscanf(s, "%lf", &out) == 1;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Resident-engine plumbing.
//
// The expensive GPU/model resources are created once and kept for the lifetime of the process
// (--daemon mode). Individual render jobs reuse them when they can and rebuild only what the new
// job invalidates:
//   * D3D12 context            - reused unconditionally.
//   * forwarder + snippet DLL  - LoadLibrary'd once; release/re-create of the feature handle
//                                 never reloads the 165 MB weight DLL (reference counting).
//   * feature (model handle)   - rebuilt when source W/H or one of the create-latched tuning
//                                 values (preset/intensity/style/...) changes.
//   * working-res textures     - rebuilt when the working resolution changes.
//   * NV-OF / DepthAnything    - rebuilt per job that needs them (they carry per-job state such
//                                 as the previous frame / depth cache that must not leak across
//                                 videos; ORT + NV-OF DLLs themselves stay cached by the OS).
//
// Return codes for a job: 0 = ok, 1 = error, -2 = cancelled by the caller.
// ---------------------------------------------------------------------------------------------
struct DaemonState {
    D3D12Ctx ctx;
    bool ctxOk = false;

    DlssNr nr;
    bool nrLoaded = false;
    NgxParams params;              // shared NGX parameter block (all keys are overwritten per use)

    ComPtr<ID3D12Resource> texColor, texOutput, texDepth, texMvec, texGrid, texFinal;
    uint32_t texW = 0, texH = 0;

    FinalBlender blend;            // GPU residual blend + 8-bit dither pass (see blend_pass.h)
    bool blendInit = false;        // init() succeeded -> GPU path available
    bool blendTargets = false;     // setTargets() bound for the current texFinal

    MvecDensify densify;           // GPU densify of the sparse NV-OF grid -> texMvec
    bool densifyInit = false;      // init() succeeded -> GPU path available
    bool densifyTargets = false;   // setTargets() bound for the current texGrid/texMvec
    uint32_t gridTexW = 0, gridTexH = 0;   // dims the cached texGrid was created at

    bool featValid = false;        // feature handle exists for featW/featH + featS
    uint32_t featW = 0, featH = 0;
    DlssNrSettings featS;
};

static bool settingsEqual(const DlssNrSettings& a, const DlssNrSettings& b) {
    return a.preset == b.preset && a.intensity == b.intensity && a.style == b.style &&
           a.localStructure == b.localStructure && a.localTone == b.localTone &&
           a.skinStructure == b.skinStructure && a.useAutoMask == b.useAutoMask &&
           a.uiCorrection == b.uiCorrection;
}

// One render job over DaemonState. `cancel` (optional) is polled once per frame; when set the job
// stops cleanly and returns -2.
static int runJob(DaemonState& st, Options& opt, const std::atomic<bool>* cancel) {
    // ---------------------------------------------------------------- probe
    VideoInfo info;
    std::string probeErr;
    if (!probeVideo(opt.input, info, probeErr)) {
        printf("ERROR probe: %s\n", probeErr.c_str());
        return 1;
    }
    if (info.fps <= 0) info.fps = 25.0;

    printf("input  : %s\n", opt.input.c_str());
    printf("        %dx%d @ %.3f fps, audio=%s\n", info.width, info.height, info.fps,
           info.hasAudio ? "yes" : "no");
    printf("output : %s (%s)\n", opt.output.c_str(), opt.encoder.c_str());

    // Model dll fallback probe (default template names only; explicit --snippet/--forwarder
    // are used verbatim). Lets a bare CLI run pick nvngx_dlssnr_fp16/_fp8 automatically.
    if (opt.snippet == "nvngx_dlssnr.dll") opt.snippet = probeModelDll(opt.snippet);
    if (opt.forwarder == "nvngx.dll_dlssnr.dll") opt.forwarder = probeModelDll(opt.forwarder);
    printf("model   : %s  <-  %s\n", opt.snippet.c_str(), opt.forwarder.c_str());

    // ---------------------------------------------------------------- d3d12 (once)
    if (!st.ctxOk) {
        if (!st.ctx.init() || !st.ctx.ok()) {
            printf("ERROR d3d12: %s\n", st.ctx.lastError());
            return 1;
        }
        st.ctxOk = true;
        // The GPU finalisation pass (residual blend + dither) shares the D3D12 device. If it
        // cannot build (e.g. missing d3dcompiler) the job falls back to the CPU finalizer.
        const char* berr = nullptr;
        st.blendInit = st.blend.init(st.ctx.dev(), &berr);
        if (!st.blendInit) {
            printf("WARNING: GPU blend pass unavailable (%s); using CPU finalizer\n",
                   berr && *berr ? berr : "unknown error");
        }
        // The motion densify pass has no CPU fallback any more (the NV-OF side only hands
        // over the sparse grid), so an NV-OF job hard-requires it (checked where NV-OF inits).
        const char* derr = nullptr;
        st.densifyInit = st.densify.init(st.ctx.dev(), &derr);
        if (!st.densifyInit) {
            printf("WARNING: GPU motion densify pass unavailable (%s); NV-OF jobs will fail\n",
                   derr && *derr ? derr : "unknown error");
        }
    }

    const UINT W = (UINT)info.width;
    const UINT H = (UINT)info.height;

    const UINT useW = W;
    const UINT useH = H;
    const UINT useRowBytes = useW * 4;
    // 10/12-bit output: the final frame must stay 16-bit per channel (fed as rgba64le to a
    // HEVC/x265 10-bit encoder), so the pipeline reads back the 16F result instead of the
    // GPU's 8-bit final pass.
    const bool deepOut = opt.pixFmt.find("10le") != std::string::npos ||
                         opt.pixFmt.find("12le") != std::string::npos ||
                         opt.pixFmt.find("16le") != std::string::npos;

    // -------------------------------------------------- working-res textures (rebuild on size)
    bool outTexFresh = false;
    if (st.texW != useW || st.texH != useH) {
        st.texColor = st.ctx.createTex(useW, useH, DXGI_FORMAT_R8G8B8A8_UNORM, false, L"nr_color");
        st.texOutput = st.ctx.createTex(useW, useH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"nr_output");
        st.texDepth = st.ctx.createTex(useW, useH, DXGI_FORMAT_R32_FLOAT, false, L"nr_depth");
        st.texMvec = st.ctx.createTex(useW, useH, DXGI_FORMAT_R16G16_FLOAT, true, L"nr_mvec");
        // Final 8-bit texture written by the blend pass (RTV + UAV so downloadTex can read it).
        st.texFinal = st.ctx.createTex(useW, useH, DXGI_FORMAT_R8G8B8A8_UNORM, true, L"nr_final",
                                       true);
        if (!st.texColor || !st.texOutput || !st.texDepth || !st.texMvec || !st.texFinal) {
            printf("ERROR d3d12: texture creation failed\n");
            return 1;
        }
        if (!st.ctx.zeroTex(st.texDepth.Get()) || !st.ctx.zeroTex(st.texMvec.Get())) {
            printf("ERROR d3d12: could not zero depth/motion\n");
            return 1;
        }
        st.texW = useW;
        st.texH = useH;
        outTexFresh = true;
        st.blendTargets = false;
    }

    // -------------------------------------------------- forwarder + feature (rebuild on W/H or tuning)
    if (!st.nrLoaded) {
        std::wstring fwPath = widen(opt.forwarder);
        if (!st.nr.load(fwPath.c_str())) {
            printf("ERROR dlssnr: %s\n", st.nr.lastError());
            return 1;
        }
        st.nrLoaded = true;
        printf("forwarder resolved: create=%d evaluate=%d release=%d set_slot=%d\n",
               (st.nr.resolved() & 1) ? 1 : 0, (st.nr.resolved() & 2) ? 1 : 0,
               (st.nr.resolved() & 4) ? 1 : 0, (st.nr.resolved() & 8) ? 1 : 0);
    }

    const bool featChanged = !st.featValid || st.featW != W || st.featH != H ||
                             !settingsEqual(st.featS, opt.nr);
    if (featChanged) {
        if (st.featValid) st.nr.release();   // frees the old feature handle, DLL stays loaded
        std::wstring snipPath = widen(opt.snippet);
        bool created = false;
        if (!st.ctx.execSync(
                [&](ID3D12GraphicsCommandList* cmd) {
                    created = st.nr.create(st.ctx.dev(), cmd, st.params.ptr(), snipPath.c_str(),
                                           W, H, opt.nr);
                },
                "create feature")) {
            printf("ERROR dlssnr: command list failed during create\n");
            st.featValid = false;
            return 1;
        }
        if (!created) {
            printf("ERROR dlssnr: feature creation failed (init=0x%08X create=0x%08X)\n",
                   (unsigned)st.nr.lastInit(), (unsigned)st.nr.lastCreate());
            st.featValid = false;
            return 1;
        }
        st.featW = W;
        st.featH = H;
        st.featS = opt.nr;
        st.featValid = true;
        printf("feature created: init=0x%08X create=0x%08X\n", (unsigned)st.nr.lastInit(),
               (unsigned)st.nr.lastCreate());
        outTexFresh = true;   // snippet leaves output writable; a fresh texture starts in COMMON
    }

    // The output texture starts in COMMON; make it writable as a UAV exactly once per (re)create.
    if (outTexFresh) {
        if (!st.ctx.execSync(
                [&](ID3D12GraphicsCommandList*) { st.ctx.transitionToWrite(st.texOutput.Get()); },
                "output to UAV")) {
            printf("ERROR d3d12: output transition failed\n");
            return 1;
        }
    }

    // Bind the blend pass to the current working textures (only when the GPU path is usable).
    if (st.blendInit && !st.blendTargets) {
        if (!st.blend.setTargets(st.ctx.dev(), st.texColor.Get(), st.texOutput.Get(),
                                 st.texFinal.Get(), useW, useH)) {
            printf("WARNING: blend setTargets failed; using CPU finalizer\n");
            st.blendInit = false;
        } else {
            st.blendTargets = true;
        }
    }
    // GPU finalisation is disabled for --png16 and deep-bit output: the 16F model result must be
    // read back (not quantised to 8-bit on the GPU) so the 16-bit export keeps full precision.
    const bool useGpuBlend = st.blendInit && st.blendTargets && st.blend.ok() && opt.png16.empty() &&
                             !deepOut;

    // ---------------------------------------------------------------- io window
    double durationSec = 0.0;
    if (opt.endTime > opt.startTime) durationSec = opt.endTime - opt.startTime;
    else if (opt.endTime > 0.0) durationSec = opt.endTime;   // start 0, end N

    // ---------------------------------------------------------------- motion/depth guides
    // NV-OF and DepthAnything keep per-job state (previous frame / depth cache), so a fresh
    // instance is built for every job that uses them; the size is always the working resolution.
    bool useNvof = (opt.frameGuidance == 3);
    bool useRealDepth = false;
    std::unique_ptr<NvofMotion> nvof;
    std::unique_ptr<DepthAnything> depthModel;
    if (useNvof) {
        nvof = std::make_unique<NvofMotion>();
        if (!nvof->init(useW, useH)) {
            printf("ERROR: NV-OF requested (--frame-guidance 3) but unavailable (%s).\n",
                   nvof->lastError());
            printf("       Re-run with --frame-guidance 0 (Force Zero motion) or fix the driver.\n");
            return 1;
        }
        printf("NVOF initialized: grid=%upx, motion source = hardware optical flow\n",
               nvof->gridSize());
        // The sparse grid texture + densify binding follow the job's NV-OF session (grid dims
        // only change with the working resolution / driver grid caps, so cache the creation).
        if (!st.densifyInit) {
            printf("ERROR: GPU motion densify unavailable; cannot densify the NV-OF grid\n");
            return 1;
        }
        const uint32_t gw = nvof->gridWidth();
        const uint32_t gh = nvof->gridHeight();
        if (!st.texGrid || st.gridTexW != gw || st.gridTexH != gh) {
            st.texGrid = st.ctx.createTex(gw, gh, DXGI_FORMAT_R16G16_SINT, true, L"nr_grid");
            st.gridTexW = gw;
            st.gridTexH = gh;
            st.densifyTargets = false;
        }
        if (!st.texGrid) {
            printf("ERROR d3d12: grid texture creation failed\n");
            return 1;
        }
        if (!st.densifyTargets) {
            if (!st.densify.setTargets(st.ctx.dev(), st.texGrid.Get(), st.texMvec.Get(), useW,
                                       useH, nvof->gridSize())) {
                printf("ERROR: densify setTargets failed\n");
                return 1;
            }
            st.densifyTargets = true;
        }
    }
    if (opt.depthInterval > 0) {
        wchar_t exeDirW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exeDirW, MAX_PATH);
        std::wstring dirW(exeDirW);
        size_t slashW = dirW.find_last_of(L"\\/");
        if (slashW != std::wstring::npos) dirW = dirW.substr(0, slashW);
        dirW += L"\\depth";
        int n = WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), (int)dirW.size(), nullptr, 0,
                                    nullptr, nullptr);
        std::string dir((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), (int)dirW.size(), &dir[0], n, nullptr, nullptr);
        depthModel = std::make_unique<DepthAnything>();
        if (depthModel->init(dir, useW, useH, (uint32_t)opt.depthInterval)) {
            useRealDepth = true;
            printf("DepthAnything initialized (interval=%d)\n", opt.depthInterval);
        } else {
            printf("WARNING: DepthAnything unavailable (%s); falling back to Sobel depth\n",
                   depthModel->lastError());
        }
    }

    // ---------------------------------------------------------------- buffers
    // Decode + NV-OF run on a producer thread while the GPU stages (upload/evaluate/blend/
    // download/encode) run on the main thread. Two slots let the producer get one frame ahead
    // so decode waits hide behind the GPU work.
    std::vector<uint8_t> outBuf((size_t)useRowBytes * useH);          // rgba8 -> encoder
    std::vector<uint8_t> outF16((size_t)useW * 8 * useH);             // CPU-finalizer fallback
    std::vector<uint16_t> out16;                                       // rgba64le for 10/12-bit
    if (deepOut) out16.resize((size_t)useW * useH * 4);
    const size_t frameBytes = (size_t)useRowBytes * useH;
    // NV-OF now hands over only the sparse flow grid (gridW*gridH*4 bytes), which the D3D12
    // densify pass up-samples to the full-res motion field; the 8MB motion buffer is gone.
    const size_t mvecBytes =
        useNvof ? (size_t)nvof->gridWidth() * nvof->gridHeight() * 4 : 0;
    std::vector<uint8_t> depthData((size_t)useW * 4 * useH);

    struct FrameSlot {
        std::vector<uint8_t> rgba;
        std::vector<uint8_t> mvec;
        std::atomic<int> state{0};   // 0 = free, 1 = filled by the producer
    };
    const int kSlots = 2;
    // Fixed array: FrameSlot carries a non-movable std::atomic, so it cannot live in a vector
    // that may reallocate. Two slots never need to move.
    std::array<FrameSlot, 2> slots;
    for (int i = 0; i < kSlots; ++i) {
        slots[i].rgba.resize(frameBytes);
        if (useNvof) slots[i].mvec.resize(mvecBytes);
    }

    DepthBuffer depthBuffer;
    std::vector<float> realDepth((size_t)useW * useH, 0.f);

    // ---------------------------------------------------------------- decode/encode pipes
    VideoReader reader;
    if (!reader.open(opt.input, (int)useW, (int)useH, info.fps, opt.startTime, durationSec,
                     opt.hwDecode)) {
        printf("ERROR: could not start the decoder\n");
        return 1;
    }
    if (reader.usedHw()) printf("decoder : NVDEC (hardware)\n");
    VideoWriter writer;
    std::string audioSrc = (opt.keepAudio && info.hasAudio) ? opt.input : std::string();
    if (!writer.open(opt.output, (int)useW, (int)useH, info.fps, opt.encoder, audioSrc,
                     opt.startTime, opt.extraArgs, opt.pixFmt, deepOut)) {
        printf("ERROR: could not start the encoder\n");
        reader.close();
        return 1;
    }

    long long total = info.nbFrames > 0 ? info.nbFrames : 0;
    if (durationSec > 0 && info.fps > 0) {
        total = (long long)(durationSec * info.fps + 0.5);
    }
    long long done = 0;
    bool stopped = false;
    auto tLastProg = std::chrono::steady_clock::now();
    bool progFirst = true;

    // ---------------------------------------------------------------- --perf stage timers
    // decode/nvof accumulate on the producer thread, the rest on the main thread. All reads
    // happen after the producer is joined, so no locking is required.
    struct Perf {
        double decode = 0, nvof = 0, depth = 0, upColor = 0, evaluate = 0, down = 0,
               residual = 0, encode = 0;
        long long n = 0;
    } perf;
    auto T = std::chrono::steady_clock::now;
    auto addNs = [](double& acc, std::chrono::steady_clock::time_point a,
                    std::chrono::steady_clock::time_point b) {
        acc += std::chrono::duration<double>(b - a).count();
    };

    // ---------------------------------------------------------------- producer thread
    // Reads source frames from ffmpeg and feeds NV-OF. Depth handling stays on the main thread
    // (it is cheap when enabled and keeps the DirectML/ORT calls single-threaded).
    std::atomic<bool> decodeStop{false};
    std::atomic<bool> eof{false};
    std::atomic<bool> prodError{false};   // producer hit a hard failure (e.g. NV-OF died)
    std::atomic<long long> decoded{0};
    bool softRetried = false;   // accessed only on the producer thread
    // Wake-up signalling between the producer thread and the main loop. Data itself still
    // travels through the atomics below; the CV just replaces the old 200us polling so neither
    // side burns time spinning (and adds latency) while the other is working.
    std::mutex cvMtx;
    std::condition_variable cv;
    auto notifyAll = [&] { cv.notify_all(); };
    std::thread producer([&] {
        long long made = 0;   // frames delivered to the ring so far (also the resume point)
        // Reads one frame from the decoder.
        //   returns 1 = frame available, 0 = clean end of the decode window,
        //             -1 = decoder failed before the window ended (message already printed).
        // A decoder pipe ending is ambiguous: ffmpeg exits 0 after producing the full -t
        // window (normal EOF), but a hardware decode failure aborts it with a non-zero
        // status. We distinguish the two via the child exit status, and only reopen in
        // software when the hardware decoder really died. The software re-open seeks to the
        // same point and skips the frames the hardware decoder already delivered, so the
        // stream position (and therefore the output) stays continuous.
        auto readOne = [&](uint8_t* dst) -> int {
            if (reader.readFrame(dst, frameBytes)) return 1;
            const int rc = [&] {
                reader.close();
                return reader.exitStatus();
            }();
            if (rc == 0) return 0;   // ffmpeg finished the window on its own: clean EOF
            if (reader.usedHw() && !softRetried) {
                softRetried = true;
                const long long skip = made;
                printf("decoder : NVDEC aborted (exit %d) mid-stream; reopening in software",
                       rc);
                if (!reader.open(opt.input, (int)useW, (int)useH, info.fps, opt.startTime,
                                 durationSec, false)) {
                    printf(" -- reopen failed\n");
                    return -1;
                }
                if (skip > 0) {
                    printf(" (skipping %lld delivered frame(s))\n", skip);
                    std::vector<uint8_t> tmp(frameBytes);
                    for (long long i = 0; i < skip; ++i) {
                        if (!reader.readFrame(tmp.data(), frameBytes)) {
                            printf("ERROR: software decoder ended while skipping frames\n");
                            reader.close();
                            return -1;
                        }
                    }
                } else {
                    printf("\n");
                }
                // Loop once more: read the resume frame from the software decoder.
                if (reader.readFrame(dst, frameBytes)) return 1;
                reader.close();
                if (reader.exitStatus() == 0) return 0;
                printf("ERROR: software decoder exited early (status %d)\n",
                       reader.exitStatus());
                return -1;
            }
            printf("ERROR: decoder exited with status %d before the window ended\n", rc);
            return -1;
        };
        for (;;) {
            if (decodeStop.load()) return;
            const int s = (int)(made % kSlots);
            {
                std::unique_lock<std::mutex> lk(cvMtx);
                cv.wait(lk, [&] {
                    return slots[s].state.load() == 0 || decodeStop.load();
                });
            }
            if (decodeStop.load()) return;
            auto t0 = T();
            const int r = readOne(slots[s].rgba.data());
            auto t1 = T();
            addNs(perf.decode, t0, t1);
            if (r != 1) {
                if (r == -1) prodError.store(true);
                eof.store(true);
                notifyAll();
                return;
            }
            if (useNvof) {
                auto tA = T();
                if (!nvof->feed(slots[s].rgba.data(), slots[s].mvec.data())) {
                    printf("ERROR: NVOF feed failed at frame %lld (%s)\n", made,
                           nvof->lastError());
                    decodeStop.store(true);
                    eof.store(true);
                    prodError.store(true);
                    notifyAll();
                    return;
                }
                auto tB = T();
                addNs(perf.nvof, tA, tB);
            }
            slots[s].state.store(1);
            decoded.store(++made);
            notifyAll();
        }
    });

    // ---------------------------------------------------------------- main render loop
    while (true) {
        if (cancel && cancel->load()) {
            printf("cancelled by user\n");
            stopped = true;
            break;
        }
        // Wait for the frame we need, or until the producer finishes (EOF/abort/cancel).
        {
            std::unique_lock<std::mutex> lk(cvMtx);
            cv.wait(lk, [&] {
                return decoded.load() > done || eof.load() || decodeStop.load() ||
                       (cancel && cancel->load());
            });
        }
        if (decoded.load() <= done) {
            if (prodError.load() && !stopped) stopped = true;   // report producer failures
            break;   // no more frames (EOF or aborted producer)
        }

        const int s = (int)(done % kSlots);
        if (slots[s].state.load() != 1) break;   // defensive; should never trigger
        const uint8_t* inP = slots[s].rgba.data();

        // ------------------------------------------------------------- first-frame dump
        if (done == 0 && !opt.dumpFrame.empty()) {
            int wn = MultiByteToWideChar(CP_UTF8, 0, opt.dumpFrame.c_str(), -1, nullptr, 0);
            std::wstring wp((size_t)wn - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, opt.dumpFrame.c_str(), -1, &wp[0], wn);
            HANDLE hf = CreateFileW(wp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                char hdr[96];
                int hlen = snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", (unsigned)useW,
                                    (unsigned)useH);
                std::vector<uint8_t> rgb((size_t)useW * useH * 3);
                for (size_t i = 0, j = 0; i < frameBytes; i += 4, j += 3) {
                    rgb[j] = inP[i];
                    rgb[j + 1] = inP[i + 1];
                    rgb[j + 2] = inP[i + 2];
                }
                DWORD wrote = 0;
                WriteFile(hf, hdr, (DWORD)hlen, &wrote, nullptr);
                WriteFile(hf, rgb.data(), (DWORD)rgb.size(), &wrote, nullptr);
                CloseHandle(hf);
                printf("dumped input frame -> %s\n", opt.dumpFrame.c_str());
            } else {
                printf("WARNING: cannot open dump path %s (err=%lu)\n", opt.dumpFrame.c_str(),
                       GetLastError());
            }
        }

        // ------------------------------------------------------------- depth guidance
        const bool depthChanged = opt.depthInterval > 0 && (done % opt.depthInterval == 0);
        auto tC = T();
        if (useRealDepth) {
            if (!depthModel->feed(inP, realDepth.data())) {
                printf("ERROR: depth inference failed at frame %lld (%s)\n", done,
                       depthModel->lastError());
                stopped = true;
                break;
            }
            if (depthChanged) {
                memcpy(depthData.data(), realDepth.data(), realDepth.size() * 4);
                if (!st.ctx.uploadTex(st.texDepth.Get(), depthData.data(), useW * 4, useH)) {
                    printf("ERROR: depth upload failed at frame %lld\n", done);
                    stopped = true;
                    break;
                }
            }
        } else if (opt.depthInterval > 0) {
            updateDepth(inP, (int)useW, (int)useH, opt.depthInterval, depthBuffer);
            if (depthChanged) {
                memcpy(depthData.data(), depthBuffer.depth.data(), depthData.size());
                if (!st.ctx.uploadTex(st.texDepth.Get(), depthData.data(), useW * 4, useH)) {
                    printf("ERROR: depth upload failed at frame %lld\n", done);
                    stopped = true;
                    break;
                }
            }
        }
        auto tD = T();
        addNs(perf.depth, tC, tD);

        // ------------------------------------------------------------- uploads (grid + color)
        // Both textures go in one command list: one GPU submit per frame for the colour frame
        // and the small sparse NV-OF grid (each submit previously drained the GPU and paid a
        // fixed cost). The densify pass turns the grid into the full-res texMvec later.
        auto tE = T();
        if (useNvof) {
            D3D12Ctx::UploadItem grid{st.texGrid.Get(), slots[s].mvec.data(),
                                      nvof->gridWidth() * 4, nvof->gridHeight()};
            D3D12Ctx::UploadItem color{st.texColor.Get(), inP, useRowBytes};
            if (!st.ctx.uploadTexN({grid, color}, useH)) {
                printf("ERROR: upload failed at frame %lld\n", done);
                stopped = true;
                break;
            }
        } else {
            D3D12Ctx::UploadItem color{st.texColor.Get(), inP, useRowBytes};
            if (!st.ctx.uploadTexN({color}, useH)) {
                printf("ERROR: upload failed at frame %lld\n", done);
                stopped = true;
                break;
            }
        }
        auto tF = T();
        addNs(perf.upColor, tE, tF);

        // ------------------------------------------------------------- densify + evaluate + blend
        int result = 0;
        auto tG = T();
        bool ok = st.ctx.execSync(
            [&](ID3D12GraphicsCommandList* cmd) {
                if (opt.bypassNr) {
                    // Diagnostic passthrough: skip DLSS NR (and its motion densify) entirely,
                    // still run the final colour path (residual 0 => pure input + dither), so
                    // banding / grid can be attributed to the model vs the colour pipeline.
                    result = 1;
                } else {
                    if (useNvof) st.densify.record(cmd);   // sparse grid -> full-res texMvec
                    result = st.nr.evaluate(cmd, st.params.ptr(), st.texColor.Get(),
                                            st.texDepth.Get(), st.texMvec.Get(),
                                            st.texOutput.Get(), useW, useH, useW, useH, opt.nr,
                                            opt.frameReset || done == 0);
                }
                if (useGpuBlend) st.blend.record(cmd, opt.bypassNr ? 0.0f : opt.residualMult);
            },
            "densify+evaluate+blend");
        auto tH = T();
        if (!opt.bypassNr) addNs(perf.evaluate, tG, tH);
        if (!ok || result != 1) {
            printf("ERROR: evaluate returned 0x%08X at frame %lld\n", (unsigned)result, done);
            stopped = true;
            break;
        }

        // ------------------------------------------------------------- download to CPU
        auto tI = T();
        bool dlOk;
        if (useGpuBlend) {
            // 8-bit final texture: residual blend + dither already ran on the GPU. texOutput
            // (16F) is never read back any more.
            dlOk = st.ctx.downloadTex(st.texFinal.Get(), outBuf.data(), useRowBytes, useH);
        } else {
            dlOk = st.ctx.downloadTex(st.texOutput.Get(), outF16.data(), useW * 8, useH);
        }
        if (!dlOk) {
            printf("ERROR: download failed at frame %lld\n", done);
            stopped = true;
            break;
        }
        auto tJ = T();
        addNs(perf.down, tI, tJ);

        // ------------------------------------------------------------- finalise (8-bit or deep)
        auto tK = T();
        if (!useGpuBlend) {
            if (deepOut) {
                // 10/12-bit: keep the residual-blended result at 16-bit per channel (0..65535,
                // no dither needed) so the encoder quantises to its own 10/12 bits.
                finalize16(inP, outF16.data(), out16.data(), (uint32_t)useW, (uint32_t)useH,
                           opt.residualMult);
            } else {
                // Fallback path: CPU residual blend + Bayer dither from the 16F readback.
                finalizeToRgba8(inP, outF16.data(), outBuf.data(), (uint32_t)useW,
                                (uint32_t)useH, opt.residualMult);
            }
        }
        if (!useGpuBlend && done == 0 && !opt.png16.empty()) {
            // 16-bit first-frame export: full 16F precision, no 8-bit quantisation at all.
            std::vector<uint16_t> rgba16((size_t)useW * useH * 4);
            finalize16(inP, outF16.data(), rgba16.data(), (uint32_t)useW, (uint32_t)useH,
                       opt.residualMult);
            if (writePng16(opt.png16, useW, useH, rgba16.data()))
                printf("wrote 16-bit PNG -> %s\n", opt.png16.c_str());
            else
                printf("WARNING: cannot write 16-bit PNG %s (is ffmpeg on PATH?)\n",
                       opt.png16.c_str());
        }
        auto tL = T();
        addNs(perf.residual, tK, tL);

        // ------------------------------------------------------------- encode
        const uint8_t* encPtr = deepOut ? (const uint8_t*)out16.data() : outBuf.data();
        const size_t encBytes = deepOut ? (size_t)useW * useH * 8 : frameBytes;
        if (!writer.writeFrame(encPtr, encBytes)) {
            printf("ERROR: encode failed at frame %lld\n", done);
            stopped = true;
            break;
        }
        auto tM = T();
        addNs(perf.encode, tL, tM);

        slots[s].state.store(0);   // hand the slot back to the decode producer
        ++done;
        perf.n++;
        notifyAll();
        auto tNowP = std::chrono::steady_clock::now();
        double sinceP = std::chrono::duration<double>(tNowP - tLastProg).count();
        if (progFirst || sinceP >= 1.0 || done >= total) {
            printf("PROGRESS %lld/%lld\n", done, total);
            fflush(stdout);
            tLastProg = tNowP;
            progFirst = false;
        }
    }

    // Stop and join the decode producer, then close the pipes.
    decodeStop.store(true);
    notifyAll();
    if (producer.joinable()) producer.join();
    writer.close();
    reader.close();

    if (opt.perf && perf.n > 0) {
        double total = perf.decode + perf.nvof + perf.depth + perf.upColor + perf.evaluate +
                       perf.down + perf.residual + perf.encode;
        double per = 1000.0 / perf.n;
        printf("PERF stages over %lld frames (ms/frame, %% of %.1f ms):\n", perf.n, total * per);
        printf("  decode    %8.2f  %5.1f%%\n", perf.decode * per, 100.0 * perf.decode / total);
        printf("  nvof      %8.2f  %5.1f%%\n", perf.nvof * per, 100.0 * perf.nvof / total);
        printf("  depth     %8.2f  %5.1f%%\n", perf.depth * per, 100.0 * perf.depth / total);
        printf("  upColor   %8.2f  %5.1f%%\n", perf.upColor * per, 100.0 * perf.upColor / total);
        printf("  evaluate  %8.2f  %5.1f%%\n", perf.evaluate * per, 100.0 * perf.evaluate / total);
        printf("  download  %8.2f  %5.1f%%\n", perf.down * per, 100.0 * perf.down / total);
        printf("  residual  %8.2f  %5.1f%%\n", perf.residual * per, 100.0 * perf.residual / total);
        printf("  encode    %8.2f  %5.1f%%\n", perf.encode * per, 100.0 * perf.encode / total);
        printf("  -- sum    %8.2f  (wall per frame incl. loop overhead)\n", total * per);
    }

    printf("DONE %lld frames\n", done);
    if (stopped && cancel && cancel->load()) return -2;
    return (done > 0 && !stopped) ? 0 : 1;
}

// Parses one daemon command line. Format (tab separated):
//   render\t<input>\t<output>\t<encoder>\t<start>\t<end>\t<preset>\t<intensity>\t<style>\t
//   <localStructure>\t<localTone>\t<skinStructure>\t<autoMask>\t<uiCorrection>\t
//   <residualMult>\t<frameGuidance>\t<depthInterval>\t<dumpFrame>
static bool parseRenderLine(const std::string& line, Options& out) {
    std::vector<std::string> f;
    size_t pos = 0, idx;
    while ((idx = line.find('\t', pos)) != std::string::npos) {
        f.push_back(line.substr(pos, idx - pos));
        pos = idx + 1;
    }
    f.push_back(line.substr(pos));
    if (f.size() < 18 || f[0] != "render") return false;
    out.input = f[1];
    out.output = f[2];
    out.encoder = f[3];
    out.startTime = atof(f[4].c_str());
    out.endTime = atof(f[5].c_str());
    out.nr.preset = atoi(f[6].c_str());
    out.nr.intensity = (float)atof(f[7].c_str());
    out.nr.style = atoi(f[8].c_str());
    out.nr.localStructure = (float)atof(f[9].c_str());
    out.nr.localTone = (float)atof(f[10].c_str());
    out.nr.skinStructure = (float)atof(f[11].c_str());
    out.nr.useAutoMask = atoi(f[12].c_str());
    out.nr.uiCorrection = atoi(f[13].c_str());
    out.residualMult = (float)atof(f[14].c_str());
    out.frameGuidance = atoi(f[15].c_str());
    out.depthInterval = atoi(f[16].c_str());
    out.dumpFrame = f[17];
    return !out.input.empty() && !out.output.empty();
}

static int runDaemon(DaemonState& st) {
    std::mutex mtx;
    std::vector<std::string> queue;
    std::atomic<bool> cancel(false);

    std::thread readerThread([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lk(mtx);
            queue.push_back(line);
        }
    });

    bool quit = false;
    while (!quit) {
        std::string cmd;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!queue.empty()) {
                cmd = queue.front();
                queue.erase(queue.begin());
            }
        }
        if (cmd.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (cmd == "quit") {
            quit = true;
            break;
        }
        if (cmd == "cancel") {
            cancel.store(true);
            printf("cancel requested\n");
            fflush(stdout);
            continue;
        }
        if (cmd.rfind("render\t", 0) == 0) {
            Options opt;
            if (!parseRenderLine(cmd, opt)) {
                printf("ERROR bad command line\n");
                fflush(stdout);
                continue;
            }
            cancel.store(false);
            printf("=== job ===\n");
            fflush(stdout);
            int rc = runJob(st, opt, &cancel);
            printf("JOB_DONE %d\n", rc);
            fflush(stdout);
            continue;
        }
        printf("ERROR unknown command: %.80s\n", cmd.c_str());
        fflush(stdout);
    }

    cancel.store(true);
    readerThread.join();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("dlss5nr starting (wmain)\n");

    Options opt;

    // Flag-only options would be skipped by the value-consuming parse loop when nothing follows
    // them, so scan for --daemon up front (e.g. "dlss5nr_engine --daemon").
    for (int i = 1; i < argc; ++i) {
        std::string flag = narrow(argv[i]);
        if (flag == "--daemon") opt.daemon = true;
        else if (flag == "--perf") opt.perf = true;
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = narrow(argv[i]);
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        }
        if (a[0] != '-' || i + 1 >= argc) continue;
        std::string v = narrow(argv[++i]);

        if (a == "--input") opt.input = v;
        else if (a == "--output") opt.output = v;
        else if (a == "--snippet") opt.snippet = v;
        else if (a == "--forwarder") opt.forwarder = v;
        else if (a == "--encoder") opt.encoder = v;
        else if (a == "--codec-args") opt.extraArgs = v;
        else if (a == "--pix-fmt") opt.pixFmt = v;
        else if (a == "--dump-frame") opt.dumpFrame = v;
        if (a == "--no-audio") { opt.keepAudio = false; --i; }
        else if (a == "--hw-decode") { opt.hwDecode = true; --i; }
        else if (a == "--daemon") { opt.daemon = true; --i; }
        else if (a == "--start-time") parseDouble(v.c_str(), opt.startTime);
        else if (a == "--end-time") parseDouble(v.c_str(), opt.endTime);
        else if (a == "--preset") parseInt(v.c_str(), opt.nr.preset);
        else if (a == "--intensity") parseFloat(v.c_str(), opt.nr.intensity);
        else if (a == "--style") parseInt(v.c_str(), opt.nr.style);
        else if (a == "--local-tone") parseFloat(v.c_str(), opt.nr.localTone);
        else if (a == "--local-structure") parseFloat(v.c_str(), opt.nr.localStructure);
        else if (a == "--skin-structure") parseFloat(v.c_str(), opt.nr.skinStructure);
        else if (a == "--auto-mask") parseInt(v.c_str(), opt.nr.useAutoMask);
        else if (a == "--ui-correction") parseInt(v.c_str(), opt.nr.uiCorrection);
        else if (a == "--frame-reset") { opt.frameReset = true; --i; }
        else if (a == "--bypass-nr") { opt.bypassNr = true; --i; }
        else if (a == "--png16") opt.png16 = v;
        else if (a == "--residual-mult") parseFloat(v.c_str(), opt.residualMult);
        else if (a == "--frame-guidance") parseInt(v.c_str(), opt.frameGuidance);
        else if (a == "--depth-interval") parseInt(v.c_str(), opt.depthInterval);
        else if (a == "--perf") { opt.perf = true; --i; }
    }

    // Single-shot CLI mode or resident daemon (--daemon). Both share DaemonState so a
    // --daemon process keeps every heavy resource (D3D12, model feature, snippet DLL) loaded
    // for the whole session and serves jobs read from stdin.
    if (opt.daemon) {
        DaemonState st;
        return runDaemon(st);
    }

    // 10-bit encoder aliases: *_10bit map to the base encoder + a 10-bit pixel format
    // (yuv420p10le; NVENC also needs the main10 profile). The deep pipeline reads the 16F
    // result back, so these outputs never touch an 8-bit quantisation.
    if (opt.encoder == "hevc_nvenc_10bit") {
        opt.encoder = "hevc_nvenc";
        if (opt.pixFmt == "yuv420p") opt.pixFmt = "yuv420p10le";
        if (opt.extraArgs.find("-profile:v") == std::string::npos)
            opt.extraArgs += " -profile:v main10";
    } else if (opt.encoder == "libx265_10bit") {
        opt.encoder = "libx265";
        if (opt.pixFmt == "yuv420p") opt.pixFmt = "yuv420p10le";
    } else if (opt.encoder == "hevc10_lossless") {
        // Master intermediate for the two-stage flow: x265 lossless 10-bit. The lossless
        // master is the export source for any later 8/10-bit encode, so re-encoding is a
        // fast transcode, never a model re-run.
        opt.encoder = "libx265";
        if (opt.pixFmt == "yuv420p") opt.pixFmt = "yuv420p10le";
        opt.extraArgs += " -preset ultrafast -x265-params lossless=1:log-level=error";
    }

    if (opt.input.empty()) {
        usage();
        return 1;
    }
    if (opt.output.empty()) {
        opt.output = makeUniqueOutput(opt.input);
    }

    DaemonState st;
    return runJob(st, opt, nullptr);
}
