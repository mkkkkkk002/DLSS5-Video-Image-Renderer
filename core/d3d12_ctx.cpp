#include "d3d12_ctx.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <dxgi1_4.h>
#include <windows.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

D3D12Ctx::~D3D12Ctx() {
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void D3D12Ctx::setError(const char* msg) {
    m_lastError = msg;
    printf("[d3d12] error: %s\n", msg);
}

// GPU selection is user-driven (web UI passes --gpu-idx). A -1 / unset still uses the
// heuristic below (NVIDIA preferred, then most dedicated VRAM) as a sensible default.
static int g_wantIdx = -1;
void d3dSetAdapter(int index) { g_wantIdx = index; }

// Count outputs physically attached to this adapter (virtual-display clones report 0).
static UINT countOutputs(IDXGIAdapter1* a) {
    UINT nOut = 0;
    for (;;) {
        IDXGIOutput* op = nullptr;
        if (a->EnumOutputs(nOut, &op) != S_OK) break;
        op->Release();
        ++nOut;
    }
    return nOut;
}

static void collectAdapters(std::vector<ComPtr<IDXGIAdapter1>>& out) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;

    struct Entry { ComPtr<IDXGIAdapter1> a; DXGI_ADAPTER_DESC1 d; UINT outs; };
    std::vector<Entry> all;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;   // Microsoft Basic Render
        all.push_back({ a, d, countOutputs(a.Get()) });
    }

    // Drop virtual-display clones: streaming/remote tools enumerate extra adapters that report
    // the SAME vendor/device/vram as the real GPU but have no outputs of their own. Rule:
    // within a (vendor,device) group, keep only members that have >=1 output when such a member
    // exists. Groups where every member has 0 outputs (e.g. a headless render card, or an
    // Optimus dGPU that renders but drives no display) are kept untouched so we never hide a
    // card that could still render. A genuine distinct GPU has its own vendor/device id and is
    // therefore unaffected by this rule.
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& e = all[i];
        bool hasOutputted = false;
        for (const auto& o : all)
            if (o.d.VendorId == e.d.VendorId && o.d.DeviceId == e.d.DeviceId && o.outs > 0) { hasOutputted = true; break; }
        if (e.outs == 0 && hasOutputted) continue;   // duplicate clone of a real outputted GPU
        out.push_back(e.a);
    }
}

static void printAdapter(unsigned i, IDXGIAdapter1* a) {
    DXGI_ADAPTER_DESC1 d;
    a->GetDesc1(&d);
    char name[128] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, (int)sizeof(name) - 1, nullptr, nullptr);
    printf("[gpu] %u: %s (vendor 0x%04x, vram %llu MB)\n",
           i, name[0] ? name : "?", (unsigned)d.VendorId,
           (unsigned long long)(d.DedicatedVideoMemory >> 20));
    fflush(stdout);
}

int d3dListGpus() {
    std::vector<ComPtr<IDXGIAdapter1>> list;
    collectAdapters(list);
    for (unsigned i = 0; i < list.size(); ++i) printAdapter(i, list[i].Get());
    return (int)list.size();
}

static IDXGIAdapter1* pickBestAdapter() {
    std::vector<ComPtr<IDXGIAdapter1>> list;
    collectAdapters(list);
    if (list.empty()) return nullptr;
    unsigned chosen = 0;
    if (g_wantIdx >= 0 && (unsigned)g_wantIdx < list.size()) {
        chosen = (unsigned)g_wantIdx;              // user explicitly picked this one
    } else {
        UINT64 bestScore = 0;                      // default heuristic
        for (unsigned i = 0; i < list.size(); ++i) {
            DXGI_ADAPTER_DESC1 d;
            list[i]->GetDesc1(&d);
            UINT64 score = (d.VendorId == 0x10DE) ? 1000000000ULL : 0;
            score += d.DedicatedVideoMemory;
            if (score > bestScore) { bestScore = score; chosen = i; }
        }
    }
    printf("[d3d12] using adapter %u:\n", chosen);
    printAdapter(chosen, list[chosen].Get());
    return list[chosen].Detach();
}

bool D3D12Ctx::init() {
    ComPtr<IDXGIAdapter1> adapter(pickBestAdapter());   // may be null -> OS default
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr) && adapter) {
        printf("[d3d12] preferred adapter failed (0x%08x), retrying with the system default\n", (unsigned)hr);
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    }
    if (FAILED(hr)) {
        setError("D3D12CreateDevice failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) {
        setError("CreateCommandQueue failed");
        return false;
    }

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc));
    if (FAILED(hr)) {
        setError("CreateCommandAllocator failed");
        return false;
    }

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc.Get(), nullptr,
                                     IID_PPV_ARGS(&m_list));
    if (FAILED(hr)) {
        setError("CreateCommandList failed");
        return false;
    }
    m_list->Close();

    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        setError("CreateFence failed");
        return false;
    }

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        setError("CreateEvent failed");
        return false;
    }

    return true;
}

bool D3D12Ctx::execSync(std::function<void(ID3D12GraphicsCommandList*)> record, const char* what) {
    HRESULT hr = m_alloc->Reset();
    if (FAILED(hr)) {
        setError("command allocator reset failed");
        return false;
    }
    hr = m_list->Reset(m_alloc.Get(), nullptr);
    if (FAILED(hr)) {
        setError("command list reset failed");
        return false;
    }

    record(m_list.Get());

    hr = m_list->Close();
    if (FAILED(hr)) {
        setError(what && *what ? what : "command list close failed");
        return false;
    }

    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    ++m_fenceValue;
    hr = m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (FAILED(hr)) {
        setError("queue signal failed");
        return false;
    }
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    return true;
}

ComPtr<ID3D12Resource> D3D12Ctx::createTex(UINT width, UINT height, DXGI_FORMAT fmt, bool allowUav,
                                           const wchar_t* name, bool allowRtv) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (allowUav) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (allowRtv) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    ComPtr<ID3D12Resource> tex;
    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&tex));
    if (FAILED(hr)) {
        setError("createTex failed");
        return nullptr;
    }
    if (name) tex->SetName(name);
    return tex;
}

ComPtr<ID3D12Resource> D3D12Ctx::makeUploadBuffer(UINT64 bytes, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buf;
    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&buf));
    if (FAILED(hr)) return nullptr;
    if (name) buf->SetName(name);
    return buf;
}

ComPtr<ID3D12Resource> D3D12Ctx::makeReadbackBuffer(UINT64 bytes, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buf;
    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&buf));
    if (FAILED(hr)) return nullptr;
    if (name) buf->SetName(name);
    return buf;
}

static void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

void D3D12Ctx::barrierTo(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                         D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    barrier(cmd, tex, before, after);
}

void D3D12Ctx::transitionToCopyDst(ID3D12Resource* tex) {
    barrier(m_list.Get(), tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
}
void D3D12Ctx::transitionToCopySrc(ID3D12Resource* tex) {
    barrier(m_list.Get(), tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
}
void D3D12Ctx::transitionToRead(ID3D12Resource* tex) {
    barrier(m_list.Get(), tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}
void D3D12Ctx::transitionToWrite(ID3D12Resource* tex) {
    barrier(m_list.Get(), tex, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool D3D12Ctx::uploadTexN(std::initializer_list<UploadItem> items, UINT height) {
    if (items.size() == 0) return true;

    UINT64 total = 0;
    for (const auto& it : items) {
        if (!it.tex || !it.src) return false;
        D3D12_RESOURCE_DESC desc = it.tex->GetDesc();
        total += (UINT64)alignPitch(it.srcRowBytes) * height;
    }
    if (total == 0) return true;

    // Reuse the cached upload buffer, growing it only when a larger frame shows up.
    if (!m_uploadBuf || m_uploadCap < total) {
        m_uploadBuf = makeUploadBuffer(total, L"nr_upload");
        if (!m_uploadBuf) {
            setError("upload buffer alloc failed");
            return false;
        }
        m_uploadCap = total;
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    if (FAILED(m_uploadBuf->Map(0, &readRange, &mapped))) {
        setError("upload map failed");
        return false;
    }
    UINT64 offset = 0;
    struct Placed {
        ID3D12Resource* tex;
        UINT64 offset;
        UINT pitch;
        DXGI_FORMAT fmt;
        UINT w;
        UINT rows;
    };
    std::vector<Placed> placed;
    placed.reserve(items.size());
    for (const auto& it : items) {
        D3D12_RESOURCE_DESC desc = it.tex->GetDesc();
        UINT dstPitch = alignPitch(it.srcRowBytes);
        const UINT rows = it.rows ? it.rows : height;
        for (UINT y = 0; y < rows; ++y) {
            memcpy(static_cast<uint8_t*>(mapped) + offset + (UINT64)y * dstPitch,
                   it.src + (UINT64)y * it.srcRowBytes, it.srcRowBytes);
        }
        placed.push_back({it.tex, offset, dstPitch, desc.Format, (UINT)desc.Width, rows});
        offset += (UINT64)dstPitch * rows;
    }
    m_uploadBuf->Unmap(0, nullptr);

    bool ok = execSync(
        [&](ID3D12GraphicsCommandList* cmd) {
            for (auto& p : placed) {
                barrier(cmd, p.tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                dstLoc.pResource = p.tex;
                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dstLoc.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                srcLoc.pResource = m_uploadBuf.Get();
                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcLoc.PlacedFootprint.Offset = p.offset;
                srcLoc.PlacedFootprint.Footprint.Format = p.fmt;
                srcLoc.PlacedFootprint.Footprint.Width = p.w;
                srcLoc.PlacedFootprint.Footprint.Height = p.rows;
                srcLoc.PlacedFootprint.Footprint.Depth = 1;
                srcLoc.PlacedFootprint.Footprint.RowPitch = p.pitch;
                cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            }
            for (auto& p : placed) {
                barrier(cmd, p.tex, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_COMMON);
            }
        },
        "uploadTexN");
    return ok;
}

bool D3D12Ctx::uploadTex(ID3D12Resource* tex, const uint8_t* src, UINT srcRowBytes, UINT height) {
    UploadItem it{tex, src, srcRowBytes};
    return uploadTexN({it}, height);
}

bool D3D12Ctx::downloadTex(ID3D12Resource* tex, uint8_t* dst, UINT dstRowBytes, UINT height) {
    if (!tex || !dst) return false;
    D3D12_RESOURCE_DESC desc = tex->GetDesc();

    UINT srcPitch = alignPitch(dstRowBytes);
    UINT64 total = (UINT64)srcPitch * height;

    // Reuse the cached readback buffer, growing it only when a larger frame shows up.
    if (!m_readbackBuf || m_readbackCap < total) {
        m_readbackBuf = makeReadbackBuffer(total, L"nr_readback");
        if (!m_readbackBuf) {
            setError("readback buffer alloc failed");
            return false;
        }
        m_readbackCap = total;
    }

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = tex;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_readbackBuf.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = desc.Format;
    dstLoc.PlacedFootprint.Footprint.Width = (UINT)desc.Width;
    dstLoc.PlacedFootprint.Footprint.Height = height;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = srcPitch;

    bool ok = execSync(
        [&](ID3D12GraphicsCommandList* cmd) {
            barrier(cmd, tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        "downloadTex");
    if (!ok) return false;

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, (SIZE_T)total};
    HRESULT hr = m_readbackBuf->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        setError("readback map failed");
        return false;
    }
    for (UINT y = 0; y < height; ++y) {
        memcpy(dst + (UINT64)y * dstRowBytes, static_cast<uint8_t*>(mapped) + (UINT64)y * srcPitch,
               dstRowBytes);
    }
    m_readbackBuf->Unmap(0, nullptr);
    return true;
}

bool D3D12Ctx::zeroTex(ID3D12Resource* tex) {
    if (!tex) return false;
    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    UINT h = (UINT)desc.Height;

    // Bytes per pixel for the formats this pipeline creates: RGBA8 = 4, RG16F = 4, R32F = 4.
    UINT bpp = 4;
    UINT rowBytes = (UINT)desc.Width * bpp;
    UINT dstPitch = alignPitch(rowBytes);
    UINT64 total = (UINT64)dstPitch * h;

    ComPtr<ID3D12Resource> upload = makeUploadBuffer(total, L"nr_zero");
    if (!upload) {
        setError("zero upload alloc failed");
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    if (FAILED(upload->Map(0, &readRange, &mapped))) {
        setError("zero map failed");
        return false;
    }
    memset(mapped, 0, (SIZE_T)total);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = tex;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = desc.Format;
    srcLoc.PlacedFootprint.Footprint.Width = (UINT)desc.Width;
    srcLoc.PlacedFootprint.Footprint.Height = h;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = dstPitch;

    return execSync(
        [&](ID3D12GraphicsCommandList* cmd) {
            barrier(cmd, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        },
        "zeroTex");
}
