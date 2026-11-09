#include "encoder.h"

#include <dxgi.h>

#include <cstring>

#include "nvEncodeAPI.h"
#include "util.h"

namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

GUID presetGuid(int preset) {
    switch (preset) {
        case 2: return NV_ENC_PRESET_P2_GUID;
        case 3: return NV_ENC_PRESET_P3_GUID;
        case 4: return NV_ENC_PRESET_P4_GUID;
        default: return NV_ENC_PRESET_P1_GUID;
    }
}

bool isNvidia(ID3D11Device* device) {
    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    return SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put()))) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())) &&
           SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == 0x10DE;
}

}  // namespace

struct NvencEncoder::Impl {
    HMODULE dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    Params params{};

    const char* lastError() const { return session && api.nvEncGetLastErrorString ? api.nvEncGetLastErrorString(session) : ""; }
};

std::unique_ptr<VideoEncoder> openEncoder(ID3D11Device* device, ID3D11Texture2D* input, const VideoEncoder::Params& params, EncoderKind kind, CodecChoice codec) {
    if (kind == EncoderKind::Nvenc || (kind == EncoderKind::Auto && codec != CodecChoice::H264 && isNvidia(device))) {
        auto nvenc = std::make_unique<NvencEncoder>();
        if (nvenc->open(device, input, params)) return nvenc;
        if (kind == EncoderKind::Nvenc) return nullptr;
    }
    if (codec != CodecChoice::H264) {
        auto hevc = std::make_unique<MfEncoder>(proto::kCodecHevc);
        if (hevc->open(device, input, params)) return hevc;
    }
    if (codec != CodecChoice::Hevc) {
        auto h264 = std::make_unique<MfEncoder>(proto::kCodecH264);
        if (h264->open(device, input, params)) return h264;
    }
    return nullptr;
}

NvencEncoder::NvencEncoder() : impl_(new Impl) {}

NvencEncoder::~NvencEncoder() {
    close();
    if (impl_->dll) FreeLibrary(impl_->dll);
    delete impl_;
}

void NvencEncoder::close() {
    Impl& e = *impl_;
    if (e.session) {
        // An empty end-of-stream picture flushes the encoder before teardown.
        NV_ENC_PIC_PARAMS end{};
        end.version = NV_ENC_PIC_PARAMS_VER;
        end.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        e.api.nvEncEncodePicture(e.session, &end);
        if (e.bitstream) e.api.nvEncDestroyBitstreamBuffer(e.session, e.bitstream);
        if (e.registered) e.api.nvEncUnregisterResource(e.session, e.registered);
        e.api.nvEncDestroyEncoder(e.session);
    }
    e.session = nullptr;
    e.bitstream = nullptr;
    e.registered = nullptr;
    codecConfig_.clear();
}

bool NvencEncoder::open(ID3D11Device* device, ID3D11Texture2D* input, const Params& params) {
    close();
    Impl& e = *impl_;
    e.params = params;

    if (!e.dll) {
        e.dll = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!e.dll) {
            logLine("encoder: nvEncodeAPI64.dll not found; is the NVIDIA driver installed?");
            return false;
        }
    }
    auto maxVersion = reinterpret_cast<MaxVersionFn>(GetProcAddress(e.dll, "NvEncodeAPIGetMaxSupportedVersion"));
    auto createInstance = reinterpret_cast<CreateInstanceFn>(GetProcAddress(e.dll, "NvEncodeAPICreateInstance"));
    if (!maxVersion || !createInstance) return false;
    uint32_t driverVersion = 0;
    maxVersion(&driverVersion);
    const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driverVersion < needed) {
        logLine("encoder: NVIDIA driver offers NVENC API %u.%u but %u.%u is needed", driverVersion >> 4, driverVersion & 15,
                NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        return false;
    }
    e.api = NV_ENCODE_API_FUNCTION_LIST{};
    e.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(&e.api) != NV_ENC_SUCCESS) return false;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams{};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.device = device;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS status = e.api.nvEncOpenEncodeSessionEx(&sessionParams, &e.session);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: could not open a session on this GPU (status %d)", status);
        e.session = nullptr;
        return false;
    }

    const GUID codec = NV_ENC_CODEC_HEVC_GUID;
    const GUID preset = presetGuid(params.preset);
    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    status = e.api.nvEncGetEncodePresetConfigEx(e.session, codec, preset, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: preset query failed (status %d): %s", status, e.lastError());
        return false;
    }

    NV_ENC_CONFIG config = presetConfig.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;
    config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    NV_ENC_RC_PARAMS& rc = config.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    rc.averageBitRate = params.bitrate;
    rc.maxBitRate = params.bitrate;
    // Two frames of buffer keeps any one frame from being far larger than average,
    // so a frame can't monopolize the cable for long.
    rc.vbvBufferSize = static_cast<uint32_t>(static_cast<uint64_t>(params.bitrate) * 2 / params.fps);
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.enableLookahead = 0;
    rc.zeroReorderDelay = 1;
    rc.enableAQ = 1;
    rc.aqStrength = 0;
    // Constant bitrate would otherwise keep spending the full budget polishing an image that already
    // looks perfect. Stopping here keeps frames small, which saves cable time and heat.
    if (params.minQp > 0) {
        rc.enableMinQP = 1;
        rc.minQP.qpInterP = rc.minQP.qpInterB = rc.minQP.qpIntra = params.minQp;
    }

    NV_ENC_CONFIG_HEVC& hevc = config.encodeCodecConfig.hevcConfig;
    hevc.level = NV_ENC_LEVEL_AUTOSELECT;
    hevc.tier = NV_ENC_TIER_HEVC_HIGH;
    hevc.chromaFormatIDC = 1;
    hevc.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    hevc.repeatSPSPPS = 0;
    hevc.maxNumRefFramesInDPB = 1;

    NV_ENC_CONFIG_HEVC_VUI_PARAMETERS& vui = hevc.hevcVUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    vui.bitstreamRestrictionFlag = 1;

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codec;
    init.presetGUID = preset;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = init.darWidth = init.maxEncodeWidth = params.width;
    init.encodeHeight = init.darHeight = init.maxEncodeHeight = params.height;
    init.frameRateNum = params.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config;
    status = e.api.nvEncInitializeEncoder(e.session, &init);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: initialize failed for %dx%d@%d (status %d): %s", params.width, params.height, params.fps, status, e.lastError());
        return false;
    }

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = input;
    reg.width = params.width;
    reg.height = params.height;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    status = e.api.nvEncRegisterResource(e.session, &reg);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: register texture failed (status %d): %s", status, e.lastError());
        return false;
    }
    e.registered = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    status = e.api.nvEncCreateBitstreamBuffer(e.session, &bitstream);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: bitstream buffer failed (status %d): %s", status, e.lastError());
        return false;
    }
    e.bitstream = bitstream.bitstreamBuffer;

    uint8_t headers[1024];
    uint32_t headerSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.spsppsBuffer = headers;
    payload.inBufferSize = sizeof(headers);
    payload.outSPSPPSPayloadSize = &headerSize;
    status = e.api.nvEncGetSequenceParams(e.session, &payload);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: could not read stream headers (status %d): %s", status, e.lastError());
        return false;
    }
    codecConfig_.assign(headers, headers + headerSize);
    logLine("encoder: HEVC %dx%d@%d, %d Mbps, preset P%d", params.width, params.height, params.fps, params.bitrate / 1000000, params.preset);
    return true;
}

bool NvencEncoder::encode(uint32_t frameId, bool forceKeyframe, std::vector<uint8_t>& out, bool* isKeyframe) {
    Impl& e = *impl_;
    if (!e.session) return false;

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = e.registered;
    NVENCSTATUS status = e.api.nvEncMapInputResource(e.session, &map);
    if (status != NV_ENC_SUCCESS) {
        logLine("encoder: map failed (status %d): %s", status, e.lastError());
        return false;
    }

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = e.params.width;
    picture.inputHeight = e.params.height;
    picture.inputBuffer = map.mappedResource;
    picture.bufferFmt = map.mappedBufferFmt;
    picture.outputBitstream = e.bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = frameId;
    if (forceKeyframe) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    status = e.api.nvEncEncodePicture(e.session, &picture);

    bool ok = status == NV_ENC_SUCCESS;
    if (!ok) {
        logLine("encoder: encode failed (status %d): %s", status, e.lastError());
    } else {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = e.bitstream;
        status = e.api.nvEncLockBitstream(e.session, &lock);
        if (status == NV_ENC_SUCCESS) {
            const uint8_t* data = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            out.assign(data, data + lock.bitstreamSizeInBytes);
            if (isKeyframe) *isKeyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
            e.api.nvEncUnlockBitstream(e.session, e.bitstream);
        } else {
            logLine("encoder: lock failed (status %d): %s", status, e.lastError());
            ok = false;
        }
    }
    e.api.nvEncUnmapInputResource(e.session, map.mappedResource);
    return ok;
}
