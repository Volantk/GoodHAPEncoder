#include "preview_renderer.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace {

const char* kShader = R"(
Texture2D<float4> srcTex : register(t0);
Texture2D<float4> hapTex : register(t1);
Texture2D<float4> alphaTex : register(t2);

cbuffer Params : register(b0) {
    float2 viewSize;
    float2 imgSize;
    float2 center;
    float zoom;
    float exposure;
    int view;
    int difference;
    float diffGain;
    int hasHap;
    int hasAlpha;
    int checker;
    float2 pad;
};

void vsMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// The Hap Q shader from the Hap reference players.
float3 hapq(int2 p) {
    float4 c = hapTex.Load(int3(p, 0));
    c.xy -= 0.50196078431373;
    float s = c.z * (255.0 / 8.0) + 1.0;
    float co = c.x / s, cg = c.y / s, y = c.w;
    return float3(y + co - cg, y + cg, y - co - cg);
}

float4 psMain(float4 pos : SV_Position) : SV_Target {
    float2 img = (pos.xy - viewSize * 0.5) / zoom + center;
    if (img.x < 0 || img.y < 0 || img.x >= imgSize.x || img.y >= imgSize.y)
        return float4(0.075, 0.078, 0.086, 1);
    int2 p = int2(img);
    float4 src = srcTex.Load(int3(p, 0));
    float4 c = src;
    if (view != 0 && hasHap) {
        c.rgb = hapq(p);
        c.a = hasAlpha ? alphaTex.Load(int3(p, 0)).r : 1.0;
    }
    if (difference && view != 0 && hasHap) {
        c.rgb = 0.5 + (c.rgb - src.rgb) * diffGain;
        c.a = 1;
    } else {
        c.rgb *= exp2(exposure);
    }
    c = saturate(c);
    if (checker && c.a < 1) {
        int2 q = int2(pos.xy) / 8;
        float bg = ((q.x + q.y) & 1) ? 0.32 : 0.22;
        c.rgb = lerp(float3(bg, bg, bg), c.rgb, c.a);
    }
    return float4(c.rgb, 1);
}
)";

struct Constants {
    float viewSize[2];
    float imgSize[2];
    float center[2];
    float zoom;
    float exposure;
    int view;
    int difference;
    float diffGain;
    int hasHap;
    int hasAlpha;
    int checker;
    float pad[2];
};
static_assert(sizeof(Constants) % 16 == 0);

} // namespace

bool PreviewRenderer::init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    dev_ = dev;
    ctx_ = ctx;
    Com<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "preview", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsb, &err))) return false;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "preview", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &psb, &err))) {
        OutputDebugStringA((const char*)err->GetBufferPointer());
        return false;
    }
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    dev_->CreateBuffer(&bd, nullptr, &cb_);
    return true;
}

void PreviewRenderer::setSource(const ghap::Frame16& f) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = f.width;
    td.Height = f.height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{f.rgba.data(), UINT(f.width * 8), 0};
    Com<ID3D11Texture2D> tex;
    srcSrv_.Reset();
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &sd, &tex))) dev_->CreateShaderResourceView(tex.Get(), nullptr, &srcSrv_);
    imgW_ = f.width;
    imgH_ = f.height;
    hasAlpha_ = false;
    for (size_t i = 3; i < f.rgba.size(); i += 4)
        if (f.rgba[i] != 65535) { hasAlpha_ = true; break; }
}

static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> makeBlockTexture(ID3D11Device* dev, const std::vector<uint8_t>& data, int w, int h,
                                                                        DXGI_FORMAT fmt, int blockBytes) {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (data.empty()) return srv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{data.data(), UINT((w / 4) * blockBytes), 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

void PreviewRenderer::setEncoded(View which, const ghap::EncodedTextures& t) {
    if (which == Source) return;
    hapSrv_[which - 1] = makeBlockTexture(dev_.Get(), t.ycocg, t.texWidth, t.texHeight, DXGI_FORMAT_BC3_UNORM, 16);
    alphaSrv_[which - 1] = makeBlockTexture(dev_.Get(), t.alpha, t.texWidth, t.texHeight, DXGI_FORMAT_BC4_UNORM, 8);
}

void PreviewRenderer::clearEncoded(View which) {
    if (which == Source) return;
    hapSrv_[which - 1].Reset();
    alphaSrv_[which - 1].Reset();
}

ID3D11ShaderResourceView* PreviewRenderer::render(int width, int height, const Params& p) {
    if (width < 1 || height < 1) return nullptr;
    if (width != rtW_ || height != rtH_) {
        rt_.Reset(); rtv_.Reset(); rtSrv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dev_->CreateTexture2D(&td, nullptr, &rt_);
        dev_->CreateRenderTargetView(rt_.Get(), nullptr, &rtv_);
        dev_->CreateShaderResourceView(rt_.Get(), nullptr, &rtSrv_);
        rtW_ = width;
        rtH_ = height;
    }
    if (!srcSrv_) {
        const float bg[4] = {0.075f, 0.078f, 0.086f, 1};
        ctx_->ClearRenderTargetView(rtv_.Get(), bg);
        return rtSrv_.Get();
    }
    int slot = p.view == Source ? 0 : p.view - 1;
    bool hasHap = p.view != Source && hapSrv_[slot];

    D3D11_MAPPED_SUBRESOURCE m;
    ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    Constants c{};
    c.viewSize[0] = float(width); c.viewSize[1] = float(height);
    c.imgSize[0] = float(imgW_); c.imgSize[1] = float(imgH_);
    c.center[0] = p.centerX; c.center[1] = p.centerY;
    c.zoom = p.zoom;
    c.exposure = p.exposure;
    c.view = p.view;
    c.difference = p.difference;
    c.diffGain = p.diffGain;
    c.hasHap = hasHap;
    c.hasAlpha = hasHap ? alphaSrv_[slot] != nullptr : hasAlpha_;
    c.checker = p.checker;
    std::memcpy(m.pData, &c, sizeof c);
    ctx_->Unmap(cb_.Get(), 0);

    ID3D11RenderTargetView* rtv = rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cb = cb_.Get();
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srvs[3] = {srcSrv_.Get(), hasHap ? hapSrv_[slot].Get() : nullptr,
                                         hasHap ? alphaSrv_[slot].Get() : nullptr};
    ctx_->PSSetShaderResources(0, 3, srvs);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->RSSetState(nullptr);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {};
    ctx_->PSSetShaderResources(0, 3, none);
    return rtSrv_.Get();
}
