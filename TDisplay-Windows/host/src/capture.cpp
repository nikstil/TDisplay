#include "capture.h"

#include <d3dcompiler.h>

#include <cstring>

namespace {

const char kVertexShader[] = R"(
cbuffer Rect : register(b0) { float4 rect; };
struct Out { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Out main(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    Out o;
    o.uv = t;
    o.position = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    return o;
}
)";

const char kPixelShader[] = R"(
Texture2D image : register(t0);
SamplerState pointSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return image.Sample(pointSampler, uv);
}
)";

using D3DCompileFn = decltype(&D3DCompile);

bool compileShader(D3DCompileFn compile, const char* source, size_t size, const char* target, Com<ID3DBlob>& blob) {
    Com<ID3DBlob> errors;
    HRESULT hr = compile(source, size, nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                         blob.put(), errors.put());
    if (FAILED(hr)) {
        logLine("capture: shader compile failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return false;
    }
    return true;
}

bool createPointerTexture(ID3D11Device* device, int width, int height, const void* pixels, Com<ID3D11Texture2D>& texture,
                          Com<ID3D11ShaderResourceView>& view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels, static_cast<UINT>(width * 4), 0};
    if (FAILED(device->CreateTexture2D(&desc, &data, texture.put()))) return false;
    return SUCCEEDED(device->CreateShaderResourceView(texture.get(), nullptr, view.put()));
}

}  // namespace

bool Capture::open(const std::wstring& gdiName) {
    close();
    gdiName_ = gdiName;

    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())))) return false;

    // Desktop Duplication has to run on the adapter that owns the output.
    Com<IDXGIAdapter1> adapter;
    Com<IDXGIOutput> output;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, adapter.put()) == S_OK; ++a) {
        Com<IDXGIOutput> candidate;
        for (UINT o = 0; adapter->EnumOutputs(o, candidate.put()) == S_OK; ++o) {
            DXGI_OUTPUT_DESC desc{};
            candidate->GetDesc(&desc);
            if (gdiName == desc.DeviceName) {
                output = candidate;
                break;
            }
        }
        if (output) break;
    }
    if (!output) {
        logLine("capture: no GPU owns %s", narrow(gdiName).c_str());
        return false;
    }
    DXGI_ADAPTER_DESC1 adapterDesc{};
    adapter->GetDesc1(&adapterDesc);
    logLine("capture: %s is on '%s' (LUID %ld:%lu)", narrow(gdiName).c_str(), narrow(adapterDesc.Description).c_str(),
            adapterDesc.AdapterLuid.HighPart, adapterDesc.AdapterLuid.LowPart);
    if (FAILED(output.as(dxgiOutput_))) return false;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // Video support is for the Media Foundation encoders. NVENC works either way.
    HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               device_.put(), nullptr, context_.put());
    }
    if (FAILED(hr)) {
        logLine("capture: D3D11CreateDevice failed (0x%08lx)", hr);
        return false;
    }
    Com<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(device_.as(dxgiDevice))) {
        dxgiDevice->SetGPUThreadPriority(7);
        dxgiDevice->SetMaximumFrameLatency(1);
    }
    Com<ID3D10Multithread> multithread;
    if (SUCCEEDED(context_.as(multithread))) multithread->SetMultithreadProtected(TRUE);

    return startDuplication() && createPipeline();
}

void Capture::close() {
    duplication_.reset();
    dxgiOutput_.reset();
    pointerColor_.reset();
    pointerColorView_.reset();
    pointerInvert_.reset();
    pointerInvertView_.reset();
    constants_.reset();
    blendAlpha_.reset();
    blendInvert_.reset();
    sampler_.reset();
    pixelShader_.reset();
    vertexShader_.reset();
    outputView_.reset();
    output_.reset();
    clean_.reset();
    context_.reset();
    device_.reset();
    pointerVisible_ = false;
    pointerHasInvert_ = false;
    pointerWidth_ = pointerHeight_ = 0;
    haveImage_ = false;
}

bool Capture::startDuplication() {
    duplication_.reset();
    Com<IDXGIOutput5> output5;
    HRESULT hr = E_NOINTERFACE;
    if (SUCCEEDED(dxgiOutput_.as(output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(device_.get(), 0, ARRAYSIZE(formats), formats, duplication_.put());
    }
    if (FAILED(hr)) hr = dxgiOutput_->DuplicateOutput(device_.get(), duplication_.put());
    if (FAILED(hr)) {
        // This keeps failing the whole time the laptop is locked, so once is enough to say so.
        if (hr != lastDuplicationError_) logLine("capture: duplication failed (0x%08lx)", hr);
        lastDuplicationError_ = hr;
        // The display itself has gone, as when its driver reloads. It comes back under a new name.
        if (hr == DXGI_ERROR_DEVICE_REMOVED) outputGone_ = true;
        return false;
    }
    lastDuplicationError_ = S_OK;
    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    if (width_ != 0 && (static_cast<int>(desc.ModeDesc.Width) != width_ || static_cast<int>(desc.ModeDesc.Height) != height_)) {
        logLine("capture: output changed size to %ux%u", desc.ModeDesc.Width, desc.ModeDesc.Height);
        width_ = desc.ModeDesc.Width;
        height_ = desc.ModeDesc.Height;
        return false;
    }
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;
    return true;
}

bool Capture::reopen() {
    return dxgiOutput_ && startDuplication();
}

bool Capture::createPipeline() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, clean_.put()))) return false;
    // Encoders that want NV12 read this with a shader to convert it.
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, output_.put()))) return false;
    if (FAILED(device_->CreateRenderTargetView(output_.get(), nullptr, outputView_.put()))) return false;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        logLine("capture: d3dcompiler_47.dll not available");
        return false;
    }
    Com<ID3DBlob> vs, ps;
    if (!compileShader(compile, kVertexShader, sizeof(kVertexShader) - 1, "vs_5_0", vs) ||
        !compileShader(compile, kPixelShader, sizeof(kPixelShader) - 1, "ps_5_0", ps)) {
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertexShader_.put())) ||
        FAILED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixelShader_.put()))) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler, sampler_.put()))) return false;

    D3D11_BLEND_DESC blend{};
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_ZERO;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(device_->CreateBlendState(&blend, blendAlpha_.put()))) return false;
    // With a white source this inverts the destination, and with black it leaves it alone,
    // which is what masked and monochrome pointers (like the text I-beam) ask for.
    target.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    target.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    if (FAILED(device_->CreateBlendState(&blend, blendInvert_.put()))) return false;

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(&buffer, nullptr, constants_.put()));
}

void Capture::updatePointerShape(UINT bufferSize) {
    if (shapeBuffer_.size() < bufferSize) shapeBuffer_.resize(bufferSize);
    UINT used = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    if (FAILED(duplication_->GetFramePointerShape(bufferSize, shapeBuffer_.data(), &used, &info))) return;

    const int width = info.Width;
    const int height = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? info.Height / 2 : info.Height;
    if (width <= 0 || height <= 0) return;
    std::vector<unsigned char> color(static_cast<size_t>(width) * height * 4, 0);
    std::vector<unsigned char> invert(static_cast<size_t>(width) * height * 4, 0);
    bool hasInvert = false;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char* c = &color[(static_cast<size_t>(y) * width + x) * 4];
            unsigned char* v = &invert[(static_cast<size_t>(y) * width + x) * 4];
            if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                memcpy(c, &shapeBuffer_[y * info.Pitch + x * 4], 4);
            } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                const unsigned char* s = &shapeBuffer_[y * info.Pitch + x * 4];
                if (s[3] == 0) {
                    c[0] = s[0];
                    c[1] = s[1];
                    c[2] = s[2];
                    c[3] = 255;
                } else {
                    v[0] = s[0];
                    v[1] = s[1];
                    v[2] = s[2];
                    v[3] = 255;
                    hasInvert = hasInvert || s[0] || s[1] || s[2];
                }
            } else {  // Monochrome: an AND mask above an XOR mask, one bit per pixel.
                const unsigned char bit = static_cast<unsigned char>(0x80 >> (x & 7));
                const bool andBit = shapeBuffer_[y * info.Pitch + x / 8] & bit;
                const bool xorBit = shapeBuffer_[(y + height) * info.Pitch + x / 8] & bit;
                if (!andBit) {
                    const unsigned char value = xorBit ? 255 : 0;
                    c[0] = c[1] = c[2] = value;
                    c[3] = 255;
                } else if (xorBit) {
                    v[0] = v[1] = v[2] = v[3] = 255;
                    hasInvert = true;
                }
            }
        }
    }

    pointerColor_.reset();
    pointerColorView_.reset();
    pointerInvert_.reset();
    pointerInvertView_.reset();
    pointerWidth_ = pointerHeight_ = 0;
    if (!createPointerTexture(device_.get(), width, height, color.data(), pointerColor_, pointerColorView_)) return;
    pointerHasInvert_ = hasInvert && createPointerTexture(device_.get(), width, height, invert.data(), pointerInvert_, pointerInvertView_);
    pointerWidth_ = width;
    pointerHeight_ = height;
}

void Capture::compose() {
    context_->CopyResource(output_.get(), clean_.get());
    if (!pointerVisible_ || pointerWidth_ == 0) return;

    const float x0 = 2.0f * pointerX_ / width_ - 1.0f;
    const float y0 = 1.0f - 2.0f * pointerY_ / height_;
    const float x1 = 2.0f * (pointerX_ + pointerWidth_) / width_ - 1.0f;
    const float y1 = 1.0f - 2.0f * (pointerY_ + pointerHeight_) / height_;
    const float rect[4] = {x0, y0, x1, y1};
    context_->UpdateSubresource(constants_.get(), 0, nullptr, rect, 0, 0);

    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ID3D11RenderTargetView* target = outputView_.get();
    ID3D11Buffer* constants = constants_.get();
    ID3D11SamplerState* sampler = sampler_.get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, &sampler);

    ID3D11ShaderResourceView* view = pointerColorView_.get();
    context_->PSSetShaderResources(0, 1, &view);
    context_->OMSetBlendState(blendAlpha_.get(), nullptr, 0xFFFFFFFF);
    context_->Draw(4, 0);
    if (pointerHasInvert_) {
        view = pointerInvertView_.get();
        context_->PSSetShaderResources(0, 1, &view);
        context_->OMSetBlendState(blendInvert_.get(), nullptr, 0xFFFFFFFF);
        context_->Draw(4, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView* noTarget = nullptr;
    context_->OMSetRenderTargets(1, &noTarget, nullptr);
}

Capture::Result Capture::next(UINT timeoutMs) {
    if (!duplication_) return Result::Lost;
    DXGI_OUTDUPL_FRAME_INFO info{};
    Com<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, resource.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (FAILED(hr)) {
        logLine("capture: AcquireNextFrame ended (0x%08lx)", hr);
        duplication_.reset();
        return Result::Lost;
    }

    bool changed = false;
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        const bool visible = info.PointerPosition.Visible != FALSE;
        if (visible != pointerVisible_ || (visible && (info.PointerPosition.Position.x != pointerX_ || info.PointerPosition.Position.y != pointerY_))) {
            changed = true;
        }
        pointerVisible_ = visible;
        pointerX_ = info.PointerPosition.Position.x;
        pointerY_ = info.PointerPosition.Position.y;
    }
    if (info.PointerShapeBufferSize != 0) {
        updatePointerShape(info.PointerShapeBufferSize);
        changed = true;
    }
    if (info.LastPresentTime.QuadPart != 0 && resource) {
        Com<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.as(texture))) {
            context_->CopyResource(clean_.get(), texture.get());
            haveImage_ = true;
            changed = true;
        }
    }
    resource.reset();
    duplication_->ReleaseFrame();

    if (!changed || !haveImage_) return Result::NoChange;
    compose();
    context_->Flush();
    return Result::NewFrame;
}
