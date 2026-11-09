#include "encoder.h"

#include <initguid.h>

#include <codecapi.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <type_traits>

#include "util.h"

namespace {

DEFINE_GUID(kEnumAdapterLuid, 0x1d39518c, 0xe220, 0x4da8, 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1);  // MFT_ENUM_ADAPTER_LUID

// Longest to wait for the encoder to say anything. A healthy one answers in milliseconds.
constexpr DWORD kEncoderWaitMs = 2000;

// One triangle that covers the whole target.
const char kVertexShader[] = R"(
float4 main(uint id : SV_VertexID) : SV_POSITION {
    return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
)";

// BT.709, limited range, which is what the tablet is told to expect.
const char kLumaShader[] = R"(
Texture2D image : register(t0);
float main(float4 position : SV_POSITION) : SV_Target {
    const float3 rgb = image.Load(int3(position.xy, 0)).rgb;
    return (16.0 + 219.0 * dot(rgb, float3(0.2126, 0.7152, 0.0722))) / 255.0;
}
)";

// A chroma sample lines up with an even luma column and sits halfway between two luma rows, which is where
// decoders assume it is. Two bilinear reads give a 1-2-1 filter across and a plain average down.
const char kChromaShader[] = R"(
Texture2D image : register(t0);
SamplerState bilinear : register(s0);
cbuffer Size : register(b0) { float2 lumaPixel; };
float2 main(float4 position : SV_POSITION) : SV_Target {
    const float2 centre = float2(position.x * 2.0 - 0.5, position.y * 2.0);
    const float3 rgb = (image.Sample(bilinear, (centre - float2(0.5, 0.0)) * lumaPixel).rgb +
                        image.Sample(bilinear, (centre + float2(0.5, 0.0)) * lumaPixel).rgb) / 2.0;
    const float2 cbcr = float2(dot(rgb, float3(-0.114572, -0.385428, 0.5)), dot(rgb, float3(0.5, -0.454153, -0.045847)));
    return (128.0 + 224.0 * cbcr) / 255.0;
}
)";

using D3DCompileFn = decltype(&D3DCompile);
using EnumerateFn = HRESULT(WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*, IMFAttributes*, IMFActivate***, UINT32*);

// Some editions of Windows ("N") leave Media Foundation out, and NVENC has to keep working on them. So nothing
// here links to it: it's loaded when first needed, the same way NVENC is.
struct MfApi {
    decltype(&MFStartup) startup = nullptr;
    decltype(&MFShutdown) shutdown = nullptr;
    decltype(&MFCreateAttributes) createAttributes = nullptr;
    decltype(&MFCreateDXGIDeviceManager) createDeviceManager = nullptr;
    decltype(&MFCreateDXGISurfaceBuffer) createSurfaceBuffer = nullptr;
    decltype(&MFCreateMediaType) createMediaType = nullptr;
    decltype(&MFCreateSample) createSample = nullptr;
    decltype(&MFCreateMemoryBuffer) createMemoryBuffer = nullptr;
    EnumerateFn enumerate = nullptr;

    bool load() {
        if (enumerate) return true;
        HMODULE dll = LoadLibraryW(L"mfplat.dll");
        if (!dll) {
            logLine("encoder: this edition of Windows doesn't include Media Foundation");
            return false;
        }
        const auto find = [dll](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
        };
        find(startup, "MFStartup");
        find(shutdown, "MFShutdown");
        find(createAttributes, "MFCreateAttributes");
        find(createDeviceManager, "MFCreateDXGIDeviceManager");
        find(createSurfaceBuffer, "MFCreateDXGISurfaceBuffer");
        find(createMediaType, "MFCreateMediaType");
        find(createSample, "MFCreateSample");
        find(createMemoryBuffer, "MFCreateMemoryBuffer");
        EnumerateFn all = nullptr;
        find(all, "MFTEnum2");
        if (!startup || !shutdown || !createAttributes || !createDeviceManager || !createSurfaceBuffer || !createMediaType || !createSample || !createMemoryBuffer || !all) {
            logLine("encoder: Media Foundation on this PC is missing something it needs");
            return false;
        }
        enumerate = all;
        return true;
    }
};

MfApi gMf;

// Hands the encoder's events to the video thread. Invoke does nothing but wake that thread up, which lets Media
// Foundation call it from wherever it likes without a detour through a work queue.
class EventSignal final : public IMFAsyncCallback {
public:
    EventSignal() { ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

    HANDLE ready() const { return ready_; }
    // The result of the Invoke that set ready().
    IMFAsyncResult* takeResult() {
        IMFAsyncResult* result = result_;
        result_ = nullptr;
        return result;
    }

    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFAsyncCallback)) {
            *out = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = InterlockedDecrement(&references_);
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
        *flags = MFASYNC_FAST_IO_PROCESSING_CALLBACK | MFASYNC_SIGNAL_CALLBACK;
        *queue = MFASYNC_CALLBACK_QUEUE_STANDARD;
        return S_OK;
    }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        // Only one wait is ever outstanding, and it starts after the last result was taken. One left unclaimed
        // can only be from a wait the video thread gave up on.
        result->AddRef();
        if (result_) result_->Release();
        result_ = result;
        SetEvent(ready_);
        return S_OK;
    }

private:
    ~EventSignal() {
        if (result_) result_->Release();
        CloseHandle(ready_);
    }

    volatile LONG references_ = 1;
    HANDLE ready_ = nullptr;
    IMFAsyncResult* result_ = nullptr;
};

bool setUint(ICodecAPI* api, const GUID& id, ULONG value) {
    VARIANT variant{};
    variant.vt = VT_UI4;
    variant.ulVal = value;
    return SUCCEEDED(api->SetValue(&id, &variant));
}

bool setBool(ICodecAPI* api, const GUID& id, bool value) {
    VARIANT variant{};
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return SUCCEEDED(api->SetValue(&id, &variant));
}

enum class Unit { ParameterSet, Filler, Other };

Unit unitKind(proto::Codec codec, uint8_t header) {
    if (codec == proto::kCodecHevc) {
        const int type = (header >> 1) & 0x3F;
        if (type >= 32 && type <= 34) return Unit::ParameterSet;  // VPS, SPS, PPS.
        return type == 38 ? Unit::Filler : Unit::Other;
    }
    const int type = header & 0x1F;
    if (type == 7 || type == 8) return Unit::ParameterSet;  // SPS, PPS.
    return type == 12 ? Unit::Filler : Unit::Other;
}

// Calls found(unit, size) for each unit of an Annex B stream, without its start code.
template <class Found>
void forEachUnit(const uint8_t* stream, size_t size, Found found) {
    size_t start = 0;
    bool inUnit = false;
    for (size_t i = 0; i + 3 <= size; ++i) {
        if (stream[i] != 0 || stream[i + 1] != 0 || stream[i + 2] != 1) continue;
        if (inUnit) found(stream + start, i - start);
        start = i + 3;
        inUnit = true;
        i += 2;
    }
    if (inUnit) found(stream + start, size - start);
}

// A unit never ends in a zero byte, so any zeros after one are padding: the zero bytes before a start code,
// and what some encoders add to make up a constant bitrate.
size_t withoutPadding(const uint8_t* unit, size_t size) {
    while (size > 0 && unit[size - 1] == 0) --size;
    return size;
}

void appendUnit(std::vector<uint8_t>& out, const uint8_t* unit, size_t size) {
    static constexpr uint8_t kStartCode[] = {0, 0, 0, 1};
    out.insert(out.end(), kStartCode, kStartCode + 4);
    out.insert(out.end(), unit, unit + size);
}

// The parameter sets from an Annex B stream, start codes included.
std::vector<uint8_t> findParameterSets(proto::Codec codec, const std::vector<uint8_t>& stream) {
    std::vector<uint8_t> found;
    forEachUnit(stream.data(), stream.size(), [&](const uint8_t* unit, size_t size) {
        size = withoutPadding(unit, size);
        if (size > 0 && unitKind(codec, unit[0]) == Unit::ParameterSet) appendUnit(found, unit, size);
    });
    return found;
}

// Hardware encoders asked for a constant bitrate pad out pictures that came out smaller: the Intel one with
// tens of kilobytes of zeros, some with filler units. Padding on the cable would only cost time and battery.
void copyWithoutPadding(proto::Codec codec, const uint8_t* stream, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    forEachUnit(stream, size, [&](const uint8_t* unit, size_t length) {
        length = withoutPadding(unit, length);
        if (length > 0 && unitKind(codec, unit[0]) != Unit::Filler) appendUnit(out, unit, length);
    });
}

bool compile(D3DCompileFn compiler, const char* source, size_t size, const char* target, Com<ID3DBlob>& blob) {
    Com<ID3DBlob> errors;
    if (FAILED(compiler(source, size, nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put()))) {
        logLine("encoder: shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return false;
    }
    return true;
}

}  // namespace

struct MfEncoder::Impl {
    Params params{};
    proto::Codec codec = proto::kCodecHevc;
    int width = 0;
    int height = 0;
    bool comStarted = false;
    bool mfStarted = false;

    // The GPU's half: BGRA in, NV12 out.
    Com<ID3D11DeviceContext> context;
    Com<ID3D11Texture2D> nv12;
    Com<ID3D11ShaderResourceView> inputView;
    Com<ID3D11RenderTargetView> lumaTarget;
    Com<ID3D11RenderTargetView> chromaTarget;
    Com<ID3D11VertexShader> vertexShader;
    Com<ID3D11PixelShader> lumaShader;
    Com<ID3D11PixelShader> chromaShader;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11Buffer> constants;

    // The encoder's half.
    Com<IMFDXGIDeviceManager> manager;
    Com<IMFActivate> activate;
    Com<IMFTransform> transform;
    Com<ICodecAPI> codecApi;
    Com<IMFMediaEventGenerator> events;
    Com<IMFSample> sample;
    EventSignal* signal = nullptr;
    bool waitingForEvent = false;
    DWORD inputStream = 0;
    DWORD outputStream = 0;
    bool providesSamples = false;
    DWORD outputSize = 0;
    int needInput = 0;
    int haveOutput = 0;
    LONGLONG nextSampleTime = 0;
    LONGLONG sampleDuration = 0;

    bool createConverter(ID3D11Device* device, ID3D11Texture2D* input);
    bool startEncoder(IMFActivate* candidate);
    void stopEncoder();
    void configure();
    void convert();
    bool nextEvent();
    bool waitFor(int& count);
    bool collectOutput(std::vector<uint8_t>& out, bool* isKeyframe);
};

MfEncoder::MfEncoder(proto::Codec codec) : impl_(new Impl) {
    codec_ = codec;
}

MfEncoder::~MfEncoder() {
    close();
    delete impl_;
}

void MfEncoder::Impl::stopEncoder() {
    if (transform) transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    sample.reset();
    events.reset();
    codecApi.reset();
    transform.reset();
    if (activate) activate->ShutdownObject();
    activate.reset();
    // It lives on while a wait is outstanding, until the encoder's shutdown lets go of it.
    if (signal) signal->Release();
    signal = nullptr;
    waitingForEvent = false;
    needInput = 0;
    haveOutput = 0;
    nextSampleTime = 0;
}

void MfEncoder::close() {
    Impl& e = *impl_;
    e.stopEncoder();
    e.manager.reset();
    e.constants.reset();
    e.sampler.reset();
    e.chromaShader.reset();
    e.lumaShader.reset();
    e.vertexShader.reset();
    e.chromaTarget.reset();
    e.lumaTarget.reset();
    e.inputView.reset();
    e.nv12.reset();
    e.context.reset();
    codecConfig_.clear();
    if (e.mfStarted) gMf.shutdown();
    e.mfStarted = false;
    if (e.comStarted) CoUninitialize();
    e.comStarted = false;
}

bool MfEncoder::Impl::createConverter(ID3D11Device* device, ID3D11Texture2D* input) {
    device->GetImmediateContext(context.put());

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, nv12.put());
    if (FAILED(hr)) {
        logLine("encoder: this GPU can't draw into an NV12 picture (0x%08lx)", hr);
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC target{};
    target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    target.Format = DXGI_FORMAT_R8_UNORM;
    hr = device->CreateRenderTargetView(nv12.get(), &target, lumaTarget.put());
    if (SUCCEEDED(hr)) {
        target.Format = DXGI_FORMAT_R8G8_UNORM;
        hr = device->CreateRenderTargetView(nv12.get(), &target, chromaTarget.put());
    }
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(input, nullptr, inputView.put());
    if (FAILED(hr)) {
        logLine("encoder: could not set up the color conversion (0x%08lx)", hr);
        return false;
    }

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        logLine("encoder: d3dcompiler_47.dll not available");
        return false;
    }
    Com<ID3DBlob> vs, luma, chroma;
    if (!compile(compileFn, kVertexShader, sizeof(kVertexShader) - 1, "vs_5_0", vs) || !compile(compileFn, kLumaShader, sizeof(kLumaShader) - 1, "ps_5_0", luma) ||
        !compile(compileFn, kChromaShader, sizeof(kChromaShader) - 1, "ps_5_0", chroma)) {
        return false;
    }
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertexShader.put())) ||
        FAILED(device->CreatePixelShader(luma->GetBufferPointer(), luma->GetBufferSize(), nullptr, lumaShader.put())) ||
        FAILED(device->CreatePixelShader(chroma->GetBufferPointer(), chroma->GetBufferSize(), nullptr, chromaShader.put()))) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDesc, sampler.put()))) return false;

    const float lumaPixel[4] = {1.0f / width, 1.0f / height, 0, 0};
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(lumaPixel);
    buffer.Usage = D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{lumaPixel, 0, 0};
    return SUCCEEDED(device->CreateBuffer(&buffer, &data, constants.put()));
}

// Capture draws with the same device, so every setting this relies on gets set each time.
void MfEncoder::Impl::convert() {
    ID3D11ShaderResourceView* view = inputView.get();
    ID3D11SamplerState* samplerState = sampler.get();
    ID3D11Buffer* constantBuffer = constants.get();
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &samplerState);
    context->PSSetConstantBuffers(0, 1, &constantBuffer);

    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    ID3D11RenderTargetView* target = lumaTarget.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShader(lumaShader.get(), nullptr, 0);
    context->Draw(3, 0);

    viewport.Width = static_cast<float>(width / 2);
    viewport.Height = static_cast<float>(height / 2);
    target = chromaTarget.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShader(chromaShader.get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* noView = nullptr;
    ID3D11RenderTargetView* noTarget = nullptr;
    context->PSSetShaderResources(0, 1, &noView);
    context->OMSetRenderTargets(1, &noTarget, nullptr);
    context->Flush();
}

// Everything here is a request: drivers differ in what they accept, and most of it the encoder works without.
void MfEncoder::Impl::configure() {
    ICodecAPI* api = codecApi.get();
    if (!setBool(api, CODECAPI_AVLowLatencyMode, true)) logLine("encoder: it refused low-latency mode, so pictures may arrive late");
    if (!setUint(api, CODECAPI_AVEncMPVDefaultBPictureCount, 0)) logLine("encoder: it refused to stop using B-frames, which hold pictures back");
    setUint(api, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    setUint(api, CODECAPI_AVEncCommonMeanBitRate, params.bitrate);
    // Two frames of buffer keeps any one frame from being far larger than average,
    // so a frame can't monopolize the cable for long.
    setUint(api, CODECAPI_AVEncCommonBufferSize, static_cast<ULONG>(static_cast<uint64_t>(params.bitrate) * 2 / params.fps));
    setUint(api, CODECAPI_AVEncVideoMaxNumRefFrame, 1);
    // The fastest setting. With the buffer above, Intel's default failed outright on a screen full of noise.
    setUint(api, CODECAPI_AVEncCommonQualityVsSpeed, 0);
    if (params.minQp > 0) setUint(api, CODECAPI_AVEncVideoMinQP, params.minQp);
    // None of these can be told "never", so a keyframe only as rarely as the encoder allows.
    static constexpr ULONG kKeyframeGaps[] = {0x7FFFFFFF, 65535, 3000, 1000};
    for (const ULONG gap : kKeyframeGaps) {
        if (setUint(api, CODECAPI_AVEncMPVGOPSize, gap)) break;
    }
}

bool MfEncoder::Impl::startEncoder(IMFActivate* candidate) {
    candidate->AddRef();
    *activate.put() = candidate;
    HRESULT hr = activate->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(transform.put()));
    if (FAILED(hr)) {
        logLine("encoder: could not start it (0x%08lx)", hr);
        return false;
    }

    Com<IMFAttributes> attributes;
    UINT32 async = 0;
    if (FAILED(transform->GetAttributes(attributes.put())) || !attributes || FAILED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) || !async) {
        logLine("encoder: it doesn't work the way hardware encoders are supposed to");
        return false;
    }
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (FAILED(transform.as(events)) || FAILED(transform.as(codecApi))) {
        logLine("encoder: it is missing an interface it needs");
        return false;
    }

    hr = transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.get()));
    if (FAILED(hr)) {
        logLine("encoder: it won't work on this GPU (0x%08lx)", hr);
        return false;
    }

    hr = transform->GetStreamIDs(1, &inputStream, 1, &outputStream);
    if (hr == E_NOTIMPL) {
        inputStream = 0;
        outputStream = 0;
    } else if (FAILED(hr)) {
        logLine("encoder: could not get its stream numbers (0x%08lx)", hr);
        return false;
    }

    configure();

    const auto describe = [&](IMFMediaType* type) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetUINT64(MF_MT_FRAME_SIZE, (static_cast<UINT64>(width) << 32) | height);
        type->SetUINT64(MF_MT_FRAME_RATE, (static_cast<UINT64>(params.fps) << 32) | 1);
        type->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (1ull << 32) | 1);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_sRGB);
    };
    // The output comes first: the input formats on offer depend on it.
    Com<IMFMediaType> output;
    gMf.createMediaType(output.put());
    describe(output.get());
    output->SetGUID(MF_MT_SUBTYPE, codec == proto::kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, params.bitrate);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, codec == proto::kCodecHevc ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8) : static_cast<UINT32>(eAVEncH264VProfile_High));
    hr = transform->SetOutputType(outputStream, output.get(), 0);
    if (FAILED(hr)) {
        logLine("encoder: it refused %dx%d at %d fps (0x%08lx)", width, height, params.fps, hr);
        return false;
    }
    Com<IMFMediaType> input;
    gMf.createMediaType(input.put());
    describe(input.get());
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = transform->SetInputType(inputStream, input.get(), 0);
    if (FAILED(hr)) {
        logLine("encoder: it refused NV12 input (0x%08lx)", hr);
        return false;
    }

    MFT_OUTPUT_STREAM_INFO info{};
    hr = transform->GetOutputStreamInfo(outputStream, &info);
    if (FAILED(hr)) return false;
    providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputSize = std::max<DWORD>(info.cbSize, 8 * 1024 * 1024);

    Com<IMFMediaBuffer> buffer;
    hr = gMf.createSurfaceBuffer(__uuidof(ID3D11Texture2D), nv12.get(), 0, FALSE, buffer.put());
    if (FAILED(hr)) {
        logLine("encoder: could not wrap the NV12 picture for it (0x%08lx)", hr);
        return false;
    }
    Com<IMF2DBuffer> buffer2d;
    DWORD length = 0;
    if (SUCCEEDED(buffer.as(buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length))) buffer->SetCurrentLength(length);
    gMf.createSample(sample.put());
    sample->AddBuffer(buffer.get());
    sampleDuration = 10000000 / params.fps;
    sample->SetSampleDuration(sampleDuration);

    signal = new EventSignal;
    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        logLine("encoder: it would not begin streaming (0x%08lx)", hr);
        return false;
    }
    return true;
}

bool MfEncoder::open(ID3D11Device* device, ID3D11Texture2D* input, const Params& params) {
    close();
    Impl& e = *impl_;
    e.params = params;
    e.codec = codec_;
    // NV12 can't describe an odd size. A tablet with one loses its last row or column of pixels.
    e.width = params.width & ~1;
    e.height = params.height & ~1;

    if (!gMf.load()) return false;
    e.comStarted = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(gMf.startup(MF_VERSION, MFSTARTUP_LITE))) {
        logLine("encoder: Media Foundation would not start");
        close();
        return false;
    }
    e.mfStarted = true;

    if (!e.createConverter(device, input)) {
        close();
        return false;
    }

    UINT token = 0;
    if (FAILED(gMf.createDeviceManager(&token, e.manager.put())) || FAILED(e.manager->ResetDevice(device, token))) {
        logLine("encoder: could not share the GPU with Media Foundation");
        close();
        return false;
    }

    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put()))) || FAILED(dxgiDevice->GetAdapter(adapter.put())) || FAILED(adapter->GetDesc(&adapterDesc))) {
        close();
        return false;
    }
    // Only this GPU's encoders: the picture is in its memory.
    Com<IMFAttributes> filter;
    gMf.createAttributes(filter.put(), 1);
    filter->SetBlob(kEnumAdapterLuid, reinterpret_cast<const UINT8*>(&adapterDesc.AdapterLuid), sizeof(LUID));
    const MFT_REGISTER_TYPE_INFO wanted{MFMediaType_Video, codec_ == proto::kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    IMFActivate** candidates = nullptr;
    UINT32 count = 0;
    if (FAILED(gMf.enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &wanted, filter.get(), &candidates, &count))) count = 0;

    const char* codecName = codec_ == proto::kCodecHevc ? "HEVC" : "H.264";
    bool started = false;
    for (UINT32 i = 0; i < count; ++i) {
        if (!started) {
            wchar_t* name = nullptr;
            UINT32 nameLength = 0;
            candidates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLength);
            logLine("encoder: trying %s", name ? narrow(name).c_str() : "an unnamed encoder");
            CoTaskMemFree(name);
            started = e.startEncoder(candidates[i]);
            if (!started) e.stopEncoder();
        }
        candidates[i]->Release();
    }
    CoTaskMemFree(candidates);
    if (!started) {
        logLine("encoder: %s has no %s hardware encoder that works through Media Foundation", narrow(adapterDesc.Description).c_str(), codecName);
        close();
        return false;
    }

    // Not every encoder says what its stream headers will be until it has made some, so ask it for a picture. The
    // input is whatever the capture holds right now, and the stream proper starts on a keyframe of its own.
    std::vector<uint8_t> first;
    bool keyframe = false;
    if (!encode(0, true, first, &keyframe)) {
        logLine("encoder: it could not make a first picture");
        close();
        return false;
    }
    codecConfig_ = findParameterSets(codec_, first);
    if (codecConfig_.empty()) {
        Com<IMFMediaType> type;
        UINT32 size = 0;
        if (SUCCEEDED(e.transform->GetOutputCurrentType(e.outputStream, type.put())) && SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
            codecConfig_.resize(size);
            type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, codecConfig_.data(), size, nullptr);
        }
    }
    if (codecConfig_.empty()) {
        logLine("encoder: its stream has no headers to give the tablet");
        close();
        return false;
    }
    logLine("encoder: %s %dx%d@%d, %d Mbps, through Media Foundation on %s", codecName, e.width, e.height, params.fps, params.bitrate / 1000000, narrow(adapterDesc.Description).c_str());
    return true;
}

bool MfEncoder::Impl::nextEvent() {
    if (!waitingForEvent) {
        const HRESULT hr = events->BeginGetEvent(signal, nullptr);
        if (FAILED(hr)) {
            logLine("encoder: could not wait for its next event (0x%08lx)", hr);
            return false;
        }
        waitingForEvent = true;
    }
    if (WaitForSingleObject(signal->ready(), kEncoderWaitMs) != WAIT_OBJECT_0) {
        logLine("encoder: nothing from it for %lu ms", kEncoderWaitMs);
        return false;
    }
    waitingForEvent = false;
    Com<IMFAsyncResult> result;
    *result.put() = signal->takeResult();
    Com<IMFMediaEvent> event;
    HRESULT hr = events->EndGetEvent(result.get(), event.put());
    if (FAILED(hr)) {
        logLine("encoder: its events stopped (0x%08lx)", hr);
        return false;
    }
    MediaEventType type = MEUnknown;
    HRESULT status = S_OK;
    event->GetType(&type);
    event->GetStatus(&status);
    if (FAILED(status) || type == MEError) {
        logLine("encoder: it reported a failure (event %lu, 0x%08lx)", type, status);
        return false;
    }
    if (type == METransformNeedInput) ++needInput;
    if (type == METransformHaveOutput) ++haveOutput;
    return true;
}

bool MfEncoder::Impl::waitFor(int& count) {
    while (count == 0) {
        if (!nextEvent()) return false;
    }
    return true;
}

bool MfEncoder::Impl::collectOutput(std::vector<uint8_t>& out, bool* isKeyframe) {
    Com<IMFSample> own;
    if (!providesSamples) {
        Com<IMFMediaBuffer> buffer;
        gMf.createSample(own.put());
        gMf.createMemoryBuffer(outputSize, buffer.put());
        own->AddBuffer(buffer.get());
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = outputStream;
        output.pSample = own.get();
        DWORD status = 0;
        HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // It has settled on a new output format, which has to be agreed to before it hands anything over.
            Com<IMFMediaType> type;
            if (FAILED(transform->GetOutputAvailableType(outputStream, 0, type.put())) || FAILED(transform->SetOutputType(outputStream, type.get(), 0))) {
                logLine("encoder: could not agree on its changed output format");
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            logLine("encoder: could not get a picture from it (0x%08lx)", hr);
            return false;
        }
        Com<IMFSample> picture;
        if (providesSamples) {
            *picture.put() = output.pSample;
        } else {
            picture = own;
        }
        Com<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        hr = picture->ConvertToContiguousBuffer(buffer.put());
        if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr)) {
            logLine("encoder: could not read a picture from it (0x%08lx)", hr);
            return false;
        }
        copyWithoutPadding(codec, data, length, out);
        buffer->Unlock();
        UINT32 cleanPoint = 0;
        picture->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
        if (isKeyframe) *isKeyframe = cleanPoint != 0;
        return true;
    }
    logLine("encoder: its output format won't settle");
    return false;
}

bool MfEncoder::encode(uint32_t frameId, bool forceKeyframe, std::vector<uint8_t>& out, bool* isKeyframe) {
    Impl& e = *impl_;
    if (!e.transform) return false;

    e.convert();
    if (!e.waitFor(e.needInput)) return false;
    if (forceKeyframe) setUint(e.codecApi.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    e.sample->SetSampleTime(e.nextSampleTime);
    e.nextSampleTime += e.sampleDuration;
    const HRESULT hr = e.transform->ProcessInput(e.inputStream, e.sample.get(), 0);
    if (FAILED(hr)) {
        logLine("encoder: it would not take a picture (0x%08lx)", hr);
        return false;
    }
    --e.needInput;
    if (!e.waitFor(e.haveOutput)) return false;
    --e.haveOutput;
    return e.collectOutput(out, isKeyframe);
}
