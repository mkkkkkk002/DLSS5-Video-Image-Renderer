#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

// NVIDIA hardware optical flow (NV-OF) via a dedicated D3D11 device, mirroring how Magpie does
// it. NV-OF's D3D11 interface needs no manual fence orchestration (unlike the D3D12 one), which
// makes it reliable for frame-by-frame offline use.
//
// The output is the *sparse* flow grid (a few hundred KB at 1080p, grid step 4), NOT the
// full-resolution motion field. Densifying the grid to full-res R16G16_FLOAT used to happen on
// this side (first on the CPU, then in a D3D11 compute shader); it now runs as a D3D12 pass that
// writes texMvec directly on the main pipeline, which removed an 8MB GPU->CPU readback + 8MB
// CPU->GPU re-upload of the motion field every frame. Only the small grid crosses to the CPU.
class NvofMotion {
public:
    NvofMotion() = default;
    ~NvofMotion();

    NvofMotion(const NvofMotion&) = delete;
    NvofMotion& operator=(const NvofMotion&) = delete;

    bool init(uint32_t w, uint32_t h);
    bool ok() const { return m_ok; }
    const char* lastError() const { return m_err.c_str(); }
    uint32_t gridSize() const { return m_grid; }
    uint32_t gridWidth() const { return m_gridW; }
    uint32_t gridHeight() const { return m_gridH; }

    // NV-OF engine quality tier (NV_OF_PERF_LEVEL). 0 = FAST (high perf, lower quality),
    // 1 = MEDIUM, 2 = SLOW (lowest perf, best quality). Default 2 (SLOW) — this is an offline
    // renderer, so the best-quality tier costs nothing in wall time that matters. Must be set
    // before init().
    void setQuality(int tier) { m_quality = tier; }
    int quality() const { return m_quality; }

    // Feed the next RGBA8 frame (w*h*4), produce the sparse flow grid (gridW*gridH, one
    // NV_OF_FLOW_VECTOR per cell, S10.5 /32 px scale) in outGrid. First frame yields zeros (no
    // previous reference yet).
    bool feed(const uint8_t* curRGBA, uint8_t* outGrid);

private:
    bool createSession();
    void destroySession();

    bool m_ok = false;
    std::string m_err;

    HMODULE m_module = nullptr;
    void* m_session = nullptr;
    std::vector<uint8_t> m_apiFuncs;  // owns NV_OF_D3D11_API_FUNCTION_LIST copy

    Microsoft::WRL::ComPtr<ID3D11Device> m_dev11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx11;

    uint32_t m_w = 0, m_h = 0;
    uint32_t m_grid = 4;
    uint32_t m_gridW = 0, m_gridH = 0;
    int m_quality = 2;              // NV_OF_PERF_LEVEL tier: 0 FAST / 1 MEDIUM / 2 SLOW

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_inTex[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_inStaging[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_flowTex;       // sparse grid output (R16G16_SINT)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_flowStaging;   // READ staging for the grid
    void* m_inHandle[2] = {};
    void* m_flowHandle = nullptr;

    int m_slot = 0;
    bool m_havePrev = false;
    std::vector<uint8_t> m_flowCpu;   // tightly packed grid rows (gridW*gridH*4)
};
