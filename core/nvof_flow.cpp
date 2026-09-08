// NVIDIA hardware optical flow via a dedicated D3D11 device, mirroring Magpie's provider.
// Outputs the sparse flow grid; dense up-sampling to a full-res motion field happens in a D3D12
// pass on the main pipeline (see densify_pass.h).

#include "nvof_flow.h"

#include <cmath>
#include <cstring>
#include <cstdio>

#include <wrl/client.h>
#include <d3d11.h>

#undef min
#undef max

#include "nvof/nvOpticalFlowD3D11.h"
#include "d3d12_ctx.h"   // d3dGetRenderAdapter: run NV-OF on the same GPU the render pipeline uses

using Microsoft::WRL::ComPtr;

static std::string to_hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "%08X", (unsigned)v);
    return b;
}

NvofMotion::~NvofMotion() { destroySession(); }

// Wraps nvOFExecute in SEH so a driver crash surfaces as an error instead of taking down the
// process. No C++ objects needing unwinding are created inside the __try.
static NV_OF_STATUS executeSafe(const NV_OF_D3D11_API_FUNCTION_LIST& fn, void* sess,
                                const NV_OF_EXECUTE_INPUT_PARAMS& in,
                                NV_OF_EXECUTE_OUTPUT_PARAMS& out) {
    NV_OF_STATUS st = NV_OF_ERR_GENERIC;
    __try {
        st = fn.nvOFExecute((NvOFHandle)sess, &in, &out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[nvof11] execute raised SEH 0x%08X\n",
                (unsigned)GetExceptionCode());
        return (NV_OF_STATUS)0xFFFFFFFF;
    }
    return st;
}

bool NvofMotion::init(uint32_t w, uint32_t h) {
    m_w = w;
    m_h = h;
    if (!createSession()) return false;
    m_ok = true;
    return true;
}

bool NvofMotion::createSession() {
    // ---- create a dedicated D3D11 device (independent of the D3D12 pipeline) ----
    // Prefer the SAME adapter the render pipeline picked (d3dSetAdapter / --gpu-idx / the
    // NVIDIA-first heuristic). NV-OF needs to run on the real NVIDIA GPU; the OS *default*
    // D3D11 adapter is often an iGPU or a virtual display on Optimus laptops / streaming
    // setups, where NV-OF creation fails with NV_OF_ERR_INVALID_DEVICE. Fall back to the
    // default adapter only when the explicit one cannot create a device.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIAdapter1> renderAdapter;
    if (IDXGIAdapter1* a = d3dGetRenderAdapter()) {
        renderAdapter = a;
        hr = D3D11CreateDevice(renderAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION, &m_dev11, nullptr, &m_ctx11);
    }
    if (FAILED(hr) || !m_dev11 || !m_ctx11) {
        m_dev11.Reset();
        m_ctx11.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                               0, D3D11_SDK_VERSION, &m_dev11, nullptr, &m_ctx11);
    }
    if (FAILED(hr) || !m_dev11 || !m_ctx11) {
        m_err = "D3D11CreateDevice failed (0x" + to_hex((uint32_t)hr) +
                "). If you have a hybrid-GPU laptop, make sure the software runs on the NVIDIA "
                "GPU (Windows Settings > System > Display > Graphics > set the engine exe to "
                "High performance).";
        return false;
    }
    if (renderAdapter) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(renderAdapter->GetDesc1(&d))) {
            char nm[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, (int)sizeof(nm) - 1, 0, 0);
            printf("[nvof11] D3D11 device on render adapter: %s\n", nm);
        }
    } else {
        printf("[nvof11] D3D11 device on system default adapter\n");
    }

    // ---- load NVOF ----
    m_module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_module) {
        m_err = "nvofapi64.dll unavailable";
        return false;
    }
    NV_OF_D3D11_API_FUNCTION_LIST fn{};
    uint32_t apiVer = NV_OF_API_VERSION;
    auto getMax = (NV_OF_STATUS(NVOFAPI*)(uint32_t*))
        GetProcAddress(m_module, "NvOFGetMaxSupportedApiVersion");
    if (getMax) getMax(&apiVer);
    auto createInst = (NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*))
        GetProcAddress(m_module, "NvOFAPICreateInstanceD3D11");
    if (!createInst) {
        m_err = "NvOFAPICreateInstanceD3D11 not found";
        return false;
    }
    NV_OF_STATUS st = createInst(apiVer, &fn);
    if (st != NV_OF_SUCCESS || !fn.nvCreateOpticalFlowD3D11 || !fn.nvOFInit || !fn.nvOFExecute ||
        !fn.nvOFRegisterResourceD3D11 || !fn.nvOFUnregisterResourceD3D11 || !fn.nvOFGetCaps) {
        m_err = "NvOFAPICreateInstanceD3D11 failed: " + std::to_string((int)st);
        return false;
    }
    // Copy the function list into owned storage. fn is a stack local; keeping only its address
    // would leave a dangling pointer that crashes the driver when feed() later calls through it.
    m_apiFuncs.resize(sizeof(fn));
    memcpy(m_apiFuncs.data(), &fn, sizeof(fn));

    st = fn.nvCreateOpticalFlowD3D11(m_dev11.Get(), m_ctx11.Get(), (NvOFHandle*)&m_session);
    if (st != NV_OF_SUCCESS || !m_session) {
        m_err = "nvCreateOpticalFlowD3D11 failed: " + std::to_string((int)st);
        return false;
    }

    // finest supported grid
    uint32_t count = 0;
    if (fn.nvOFGetCaps((NvOFHandle)m_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &count) !=
            NV_OF_SUCCESS || !count || count > 8) {
        m_err = "grid caps query failed";
        return false;
    }
    std::vector<uint32_t> grids(count);
    fn.nvOFGetCaps((NvOFHandle)m_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &count);
    m_grid = 4;
    for (uint32_t g : grids) {
        if (g == 1) { m_grid = 1; break; }
        if (g == 2) m_grid = 2;
    }

    NV_OF_INIT_PARAMS init{};
    init.width = m_w;
    init.height = m_h;
    init.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)m_grid;
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    // Quality tier (--mvec-quality 0/1/2). SLOW (5) is documented as "lowest performance, best
    // quality" — its iterative refinement measurably cuts the flow noise on low-texture surfaces
    // that DLSSNR otherwise amplifies into shimmer. FAST/MEDIUM exist for users who prioritise
    // speed over flow accuracy.
    static const NV_OF_PERF_LEVEL kPerf[3] = {NV_OF_PERF_LEVEL_FAST, NV_OF_PERF_LEVEL_MEDIUM,
                                              NV_OF_PERF_LEVEL_SLOW};
    const int tier = (m_quality < 0) ? 0 : (m_quality > 2 ? 2 : m_quality);
    init.perfLevel = kPerf[tier];
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.hPrivData = nullptr;
    init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;  // BGRA8 textures
    st = fn.nvOFInit((NvOFHandle)m_session, &init);
    if (st != NV_OF_SUCCESS) {
        m_err = "nvOFInit failed: " + std::to_string((int)st);
        return false;
    }

    // ---- two alternating input BGRA textures (official sample binds RT|SRV|UAV) ----
    D3D11_TEXTURE2D_DESC id{};
    id.Width = m_w;
    id.Height = m_h;
    id.MipLevels = 1;
    id.ArraySize = 1;
    id.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    id.SampleDesc.Count = 1;
    id.Usage = D3D11_USAGE_DEFAULT;
    id.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(m_dev11->CreateTexture2D(&id, nullptr, &m_inTex[i]))) {
            m_err = "nvof input tex alloc failed";
            return false;
        }
        D3D11_TEXTURE2D_DESC sd = id;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_dev11->CreateTexture2D(&sd, nullptr, &staging))) {
            m_err = "nvof input staging alloc failed";
            return false;
        }
        m_inStaging[i] = staging;
    }

    // ---- sparse flow grid output texture (R16G16_SINT) + READ staging ----
    m_gridW = (m_w + m_grid - 1) / m_grid;
    m_gridH = (m_h + m_grid - 1) / m_grid;
    D3D11_TEXTURE2D_DESC fd = id;   // keep the RT|SRV|UAV bind flags: NV-OF writes it as a buffer
    fd.Width = m_gridW;
    fd.Height = m_gridH;
    fd.Format = DXGI_FORMAT_R16G16_SINT;
    if (FAILED(m_dev11->CreateTexture2D(&fd, nullptr, &m_flowTex))) {
        m_err = "nvof flow tex alloc failed";
        return false;
    }
    D3D11_TEXTURE2D_DESC fsd = fd;
    fsd.Usage = D3D11_USAGE_STAGING;
    fsd.BindFlags = 0;             // staging textures must not carry bind flags
    fsd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_dev11->CreateTexture2D(&fsd, nullptr, &m_flowStaging))) {
        m_err = "nvof flow staging alloc failed";
        return false;
    }

    // register resources
    for (int i = 0; i < 2; ++i) {
        st = fn.nvOFRegisterResourceD3D11((NvOFHandle)m_session, m_inTex[i].Get(),
                                          (NvOFGPUBufferHandle*)&m_inHandle[i]);
        if (st != NV_OF_SUCCESS || !m_inHandle[i]) {
            m_err = "nvOFRegisterResourceD3D11(input) failed: " + std::to_string((int)st);
            return false;
        }
    }
    st = fn.nvOFRegisterResourceD3D11((NvOFHandle)m_session, m_flowTex.Get(),
                                      (NvOFGPUBufferHandle*)&m_flowHandle);
    if (st != NV_OF_SUCCESS || !m_flowHandle) {
        m_err = "nvOFRegisterResourceD3D11(flow) failed: " + std::to_string((int)st);
        return false;
    }

    m_flowCpu.resize((size_t)m_gridW * m_gridH * 4);
    return true;
}

bool NvofMotion::feed(const uint8_t* curRGBA, uint8_t* outGrid) {
    if (!m_ok || !curRGBA || !outGrid) return false;
    auto& fn = *reinterpret_cast<NV_OF_D3D11_API_FUNCTION_LIST*>(m_apiFuncs.data());

    const size_t rowSrc = (size_t)m_w * 4;

    // RGBA -> BGRA straight into the mapped upload staging (per row).
    D3D11_MAPPED_SUBRESOURCE inMap{};
    if (FAILED(m_ctx11->Map(m_inStaging[m_slot].Get(), 0, D3D11_MAP_WRITE, 0, &inMap))) {
        m_err = "nvof input staging map failed";
        return false;
    }
    for (uint32_t y = 0; y < m_h; ++y) {
        const uint8_t* s = curRGBA + (size_t)y * rowSrc;
        uint8_t* d = (uint8_t*)inMap.pData + (size_t)y * inMap.RowPitch;
        for (uint32_t x = 0; x < m_w; ++x) {
            const size_t i = (size_t)x * 4;
            d[i + 0] = s[i + 2];
            d[i + 1] = s[i + 1];
            d[i + 2] = s[i + 0];
            d[i + 3] = s[i + 3];
        }
    }
    m_ctx11->Unmap(m_inStaging[m_slot].Get(), 0);
    m_ctx11->CopyResource(m_inTex[m_slot].Get(), m_inStaging[m_slot].Get());
    m_ctx11->Flush();  // ensure the copy is submitted before NVOF reads the texture

    if (!m_havePrev) {
        // hold as reference, output a zero grid (densifies to a zero motion field)
        memset(outGrid, 0, (size_t)m_gridW * m_gridH * 4);
        m_slot = 1 - m_slot;
        m_havePrev = true;
        return true;
    }

    NV_OF_EXECUTE_INPUT_PARAMS in{};
    in.inputFrame = (NvOFGPUBufferHandle)m_inHandle[m_slot];
    in.referenceFrame = (NvOFGPUBufferHandle)m_inHandle[1 - m_slot];
    in.disableTemporalHints = NV_OF_FALSE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out{};
    out.outputBuffer = (NvOFGPUBufferHandle)m_flowHandle;
    NV_OF_STATUS st = executeSafe(fn, m_session, in, out);
    if (st != NV_OF_SUCCESS) {
        if (st == (NV_OF_STATUS)0xFFFFFFFF) m_err = "nvOFExecute crashed (SEH)";
        else m_err = "nvOFExecute failed: " + std::to_string((int)st);
        return false;
    }

    // Read the sparse grid back (gridW*gridH*4 bytes, ~0.5MB at 1080p with step 4). The D3D12
    // densify pass turns this into the full-res motion field without an 8MB round trip.
    m_ctx11->CopyResource(m_flowStaging.Get(), m_flowTex.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_ctx11->Map(m_flowStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        m_err = "nvof staging map failed";
        return false;
    }
    const uint8_t* src = (const uint8_t*)map.pData;
    for (uint32_t y = 0; y < m_gridH; ++y) {
        memcpy(m_flowCpu.data() + (size_t)y * m_gridW * 4, src + (size_t)y * map.RowPitch,
               (size_t)m_gridW * 4);
    }
    m_ctx11->Unmap(m_flowStaging.Get(), 0);
    memcpy(outGrid, m_flowCpu.data(), (size_t)m_gridW * m_gridH * 4);

    m_slot = 1 - m_slot;
    return true;
}

void NvofMotion::destroySession() {
    if (!m_apiFuncs.empty()) {
        auto& fn = *reinterpret_cast<NV_OF_D3D11_API_FUNCTION_LIST*>(m_apiFuncs.data());
        if (m_flowHandle && fn.nvOFUnregisterResourceD3D11)
            fn.nvOFUnregisterResourceD3D11((NvOFGPUBufferHandle)m_flowHandle);
        for (int i = 0; i < 2; ++i) {
            if (m_inHandle[i] && fn.nvOFUnregisterResourceD3D11)
                fn.nvOFUnregisterResourceD3D11((NvOFGPUBufferHandle)m_inHandle[i]);
            m_inHandle[i] = nullptr;
        }
        if (m_session && fn.nvOFDestroy) fn.nvOFDestroy((NvOFHandle)m_session);
    }
    if (m_module) {
        FreeLibrary(m_module);
        m_module = nullptr;
    }
    m_session = nullptr;
    m_flowHandle = nullptr;
}
