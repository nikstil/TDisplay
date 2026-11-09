// Captures one Windows display with Desktop Duplication and draws the pointer
// into the captured image, ready for the encoder.
#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>

#include <string>
#include <vector>

#include "util.h"

class Capture {
public:
    enum class Result {
        NewFrame,  // output() holds a new image.
        NoChange,  // Nothing changed within the timeout.
        Lost,      // The duplication ended, for example on a desktop or mode switch. Call reopen().
    };

    // Creates a D3D11 device on the GPU that owns gdiName (\\.\DISPLAYn) and starts duplicating it.
    bool open(const std::wstring& gdiName);
    // Restarts the duplication on the same device. False if the output has a different size or is gone.
    bool reopen();
    void close();

    // True after a GPU driver reset or crash; everything has to be created again.
    bool deviceLost() const { return outputGone_ || (device_ && device_->GetDeviceRemovedReason() != S_OK); }

    Result next(UINT timeoutMs);

    ID3D11Device* device() const { return device_.get(); }
    ID3D11Texture2D* output() const { return output_.get(); }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool startDuplication();
    bool createPipeline();
    void updatePointerShape(UINT bufferSize);
    void compose();

    std::wstring gdiName_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutput1> dxgiOutput_;
    Com<IDXGIOutputDuplication> duplication_;
    int width_ = 0;
    int height_ = 0;

    Com<ID3D11Texture2D> clean_;   // The desktop without the pointer.
    Com<ID3D11Texture2D> output_;  // The desktop with the pointer; the encoder reads this.
    Com<ID3D11RenderTargetView> outputView_;

    // Pointer drawing.
    Com<ID3D11VertexShader> vertexShader_;
    Com<ID3D11PixelShader> pixelShader_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11BlendState> blendAlpha_;
    Com<ID3D11BlendState> blendInvert_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11Texture2D> pointerColor_;
    Com<ID3D11ShaderResourceView> pointerColorView_;
    Com<ID3D11Texture2D> pointerInvert_;
    Com<ID3D11ShaderResourceView> pointerInvertView_;
    std::vector<unsigned char> shapeBuffer_;
    bool pointerHasInvert_ = false;
    bool pointerVisible_ = false;
    int pointerX_ = 0;
    int pointerY_ = 0;
    int pointerWidth_ = 0;
    int pointerHeight_ = 0;
    bool haveImage_ = false;
    bool outputGone_ = false;
    HRESULT lastDuplicationError_ = S_OK;
};
