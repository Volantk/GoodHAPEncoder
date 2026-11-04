#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include "frame_encoder.h"
#include "media_reader.h"

// Draws the preview into an offscreen texture that ImGui shows as an image.
// Encoded frames are uploaded as real BC3/BC4 textures and decoded by the GPU
// with the Hap Q shader formula, the same path a media server takes.
class PreviewRenderer {
public:
    enum View { Source = 0, Ours = 1, FFmpeg = 2 };

    struct Params {
        View view = Ours;
        bool difference = false;
        float diffGain = 16.0f;
        float exposure = 0.0f; // stops
        float zoom = 1.0f;     // screen pixels per image pixel
        float centerX = 0, centerY = 0; // image coordinate at the view centre
        bool checker = true;  // composite alpha over a checkerboard
    };

    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void setSource(const ghap::Frame16& f);
    void setEncoded(View which, const ghap::EncodedTextures& t);
    void clearEncoded(View which);
    // Renders at the given size; returns the shader resource to hand to ImGui.
    ID3D11ShaderResourceView* render(int width, int height, const Params& p);

    int imageWidth() const { return imgW_; }
    int imageHeight() const { return imgH_; }
    bool hasImage() const { return srcSrv_ != nullptr; }
    bool hasAlpha() const { return hasAlpha_; }
    bool hasEncoded(View v) const { return v == Source ? hasImage() : hapSrv_[v - 1] != nullptr; }

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    Com<ID3D11Device> dev_;
    Com<ID3D11DeviceContext> ctx_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11Buffer> cb_;
    Com<ID3D11Texture2D> rt_;
    Com<ID3D11RenderTargetView> rtv_;
    Com<ID3D11ShaderResourceView> rtSrv_;
    int rtW_ = 0, rtH_ = 0;
    Com<ID3D11ShaderResourceView> srcSrv_;
    Com<ID3D11ShaderResourceView> hapSrv_[2], alphaSrv_[2];
    int imgW_ = 0, imgH_ = 0;
    bool hasAlpha_ = false;
};
