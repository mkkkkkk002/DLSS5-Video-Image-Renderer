#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

using Microsoft::WRL::ComPtr;

// Minimal synchronous D3D12 context for an offscreen NGX pipeline.
//
// Everything here is synchronous: record a command list, execute it, wait on a fence. There is
// no reason to pipeline an offline video pass, and the alternative is a fence ring buffer that
// buys nothing when the CPU is feeding frames one at a time anyway.
void d3dSetAdapter(int index);
int  d3dListGpus();

class D3D12Ctx {
public:
    ~D3D12Ctx();

    bool init();
    bool ok() const { return m_device != nullptr; }
    ID3D12Device* dev() { return m_device.Get(); }
    ID3D12CommandQueue* queue() { return m_queue.Get(); }
    ID3D12GraphicsCommandList* list() { return m_list.Get(); }

    // Record a command list, execute it, wait for the GPU to drain.
    bool execSync(std::function<void(ID3D12GraphicsCommandList*)> record, const char* what = "");

    // Creates a 2D texture in the default heap. allowUav is required for anything NGX writes to;
    // allowRtv additionally enables it as a render target (for the final blend pass). A texture
    // can carry both flags.
    ComPtr<ID3D12Resource> createTex(UINT width, UINT height, DXGI_FORMAT fmt, bool allowUav,
                                     const wchar_t* name = nullptr, bool allowRtv = false);

    // Inserts a transition barrier on an open command list (used by helper passes that run
    // inside an execSync record callback).
    static void barrierTo(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                          D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    // One texture to copy in a batched upload (see uploadTexN).
    struct UploadItem {
        ID3D12Resource* tex = nullptr;
        const uint8_t* src = nullptr;   // tightly packed rows
        UINT srcRowBytes = 0;
        UINT rows = 0;                  // row count for this item; 0 = use the height argument
    };

    // Uploads several textures in a single command list (one GPU submit per frame instead of
    // one per texture). `height` is the default row count for items with rows == 0, so textures
    // of different heights (e.g. the colour frame and the small NV-OF grid) can share a batch.
    // The upload buffer is cached and reused across frames.
    bool uploadTexN(std::initializer_list<UploadItem> items, UINT height);

    // Uploads tightly packed rows; the destination is padded to the D3D12 pitch alignment.
    bool uploadTex(ID3D12Resource* tex, const uint8_t* src, UINT srcRowBytes, UINT height);

    // Downloads to tightly packed rows, removing the destination pitch padding. The readback
    // buffer is cached and reused across frames.
    bool downloadTex(ID3D12Resource* tex, uint8_t* dst, UINT dstRowBytes, UINT height);

    // Fills a texture with zeros, for the motion-vector and depth inputs under Force Zero.
    bool zeroTex(ID3D12Resource* tex);

    // Transitions the four NGX resources. The snippet expects colour, depth and motion in a
    // readable state and the output writable as a UAV.
    void transitionToWrite(ID3D12Resource* tex);
    void transitionToRead(ID3D12Resource* tex);
    void transitionToCopySrc(ID3D12Resource* tex);
    void transitionToCopyDst(ID3D12Resource* tex);

    static UINT alignPitch(UINT rowBytes) { return (rowBytes + 255u) & ~255u; }

    const char* lastError() const { return m_lastError; }

private:
    void setError(const char* msg);
    ComPtr<ID3D12Resource> makeUploadBuffer(UINT64 bytes, const wchar_t* name);
    ComPtr<ID3D12Resource> makeReadbackBuffer(UINT64 bytes, const wchar_t* name);

    // Cross-frame caches so the per-frame upload/download stops allocating two GPU buffers
    // and destroying them on every frame (COM resource churn was visible in the perf trace).
    ComPtr<ID3D12Resource> m_uploadBuf;    // grows as needed, reused by every uploadTexN
    UINT64 m_uploadCap = 0;
    ComPtr<ID3D12Resource> m_readbackBuf;  // grows as needed, reused by every downloadTex
    UINT64 m_readbackCap = 0;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_alloc;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    const char* m_lastError = "";
};
