#include "blend_pass.h"
#include "d3d12_ctx.h"

#include <cstdio>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

namespace {

const char* kShader = R"(
Texture2D<float4> texColor : register(t0);
Texture2D<float4> texNR    : register(t1);
cbuffer Blend : register(b0) { float residualMult; float3 _pad; }

struct PSIn { float4 pos : SV_Position; };

PSIn vsMain(uint id : SV_VertexID) {
    PSIn o;
    float2 xy = float2(float((id << 1) & 2), float(id & 2));
    o.pos = float4(xy * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

// Same 4x4 Bayer matrix the CPU finalizer used, so the dither pattern is unchanged.
static const int kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

float4 psMain(PSIn i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float m = clamp(residualMult, 0.0f, 2.0f);
    float wIn = 1.0f - m;
    float3 col = texNR.Load(int3(p, 0)).rgb;          // half float 0..1
    if (wIn != 0.0f) {
        float3 inc = texColor.Load(int3(p, 0)).rgb;   // unorm 0..1
        col = wIn * inc + m * col;
    }
    col = saturate(col);
    int idx = (p.y & 3) * 4 + (p.x & 3);
    float d = ((float)kBayer[idx] + 0.5f) / 16.0f - 0.5f;
    float3 q = col * 255.0f + d + 0.5f;
    int3 qi = clamp(int3(q), int3(0, 0, 0), int3(255, 255, 255));
    return float4(qi / 255.0f, 1.0f);
}
)";

}  // namespace

FinalBlender::~FinalBlender() = default;

bool FinalBlender::init(ID3D12Device* device, const char** errOut) {
    if (errOut) *errOut = "";
    auto fail = [&](const char* m) {
        if (errOut) *errOut = m;
        printf("[blend] error: %s\n", m);
        return false;
    };

    ComPtr<ID3DBlob> vs, ps, errBlob;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vsMain",
                            "vs_5_0", 0, 0, &vs, &errBlob);
    if (FAILED(hr)) {
        return fail(errBlob ? (const char*)errBlob->GetBufferPointer()
                            : "VS compile failed");
    }
    hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "psMain",
                    "ps_5_0", 0, 0, &ps, &errBlob);
    if (FAILED(hr)) {
        return fail(errBlob ? (const char*)errBlob->GetBufferPointer()
                            : "PS compile failed");
    }

    // Root signature: root constant (residualMult @ b0) + SRV table t0/t1.
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 1;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return fail(rsErr ? (const char*)rsErr->GetBufferPointer()
                          : "root signature serialize failed");
    }
    hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr)) return fail("CreateRootSignature failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psDesc = {};
    psDesc.pRootSignature = m_rootSig.Get();
    psDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    // Rasterizer: solid fill, no culling. Rest zeroed (D3D12_DEFAULT is a d3dx12.h helper that
    // this file deliberately avoids pulling in).
    psDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psDesc.RasterizerState.DepthClipEnable = TRUE;
    psDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psDesc.SampleMask = UINT_MAX;
    psDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psDesc.NumRenderTargets = 1;
    psDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psDesc.SampleDesc.Count = 1;

    hr = device->CreateGraphicsPipelineState(&psDesc, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr)) return fail("CreateGraphicsPipelineState failed");
    return true;
}

bool FinalBlender::setTargets(ID3D12Device* device, ID3D12Resource* texColor,
                              ID3D12Resource* texNR, ID3D12Resource* texOut, uint32_t width,
                              uint32_t height) {
    m_ready = false;
    m_w = width;
    m_h = height;
    m_firstRender = true;
    m_texColor = texColor;
    m_texNR = texNR;
    m_texOut = texOut;

    if (!m_rootSig || !texColor || !texNR || !texOut) return false;

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 2;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(m_texColor.Get(), &srv, cpu);
    cpu.ptr += inc;
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(m_texNR.Get(), &srv, cpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(m_texOut.Get(), nullptr, rtv);

    m_ready = true;
    return true;
}

void FinalBlender::record(ID3D12GraphicsCommandList* cmd, float residualMult) {
    if (!m_ready) return;
    using S = D3D12_RESOURCE_STATES;
    D3D12Ctx::barrierTo(cmd, m_texColor.Get(), S::D3D12_RESOURCE_STATE_COMMON,
                        S::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12Ctx::barrierTo(cmd, m_texNR.Get(), S::D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        S::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12Ctx::barrierTo(cmd, m_texOut.Get(),
                        m_firstRender ? S::D3D12_RESOURCE_STATE_COMMON
                                      : S::D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        S::D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_firstRender = false;

    D3D12_VIEWPORT vp = {0.f, 0.f, (float)m_w, (float)m_h, 0.f, 1.f};
    D3D12_RECT rc = {0, 0, (LONG)m_w, (LONG)m_h};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &rc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 1, &residualMult, 0);

    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootDescriptorTable(
        1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    cmd->OMSetRenderTargets(1, &m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), FALSE, nullptr);
    cmd->SetPipelineState(m_pso.Get());
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12Ctx::barrierTo(cmd, m_texColor.Get(), S::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        S::D3D12_RESOURCE_STATE_COMMON);
    D3D12Ctx::barrierTo(cmd, m_texNR.Get(), S::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        S::D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12Ctx::barrierTo(cmd, m_texOut.Get(), S::D3D12_RESOURCE_STATE_RENDER_TARGET,
                        S::D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
