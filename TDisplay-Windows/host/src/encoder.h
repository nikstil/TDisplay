// Hardware video encoders that read the capture texture on the GPU, so a frame is never copied through
// system memory. All are tuned for lowest latency: no B-frames, no lookahead, one frame in and one frame
// out, and a keyframe only when asked for one.
#pragma once
#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "protocol.h"

class VideoEncoder {
public:
    struct Params {
        int width;
        int height;
        int fps;
        int bitrate;
        int preset;  // 1 (fastest) to 4.
        int minQp;   // Quality past which more bits are pointless. 0 turns this off.
    };

    virtual ~VideoEncoder() = default;

    // input must stay alive and be the texture passed for every encode.
    virtual bool open(ID3D11Device* device, ID3D11Texture2D* input, const Params& params) = 0;
    virtual void close() = 0;

    // Encodes the current contents of input into out. Blocks until the GPU is done.
    virtual bool encode(uint32_t frameId, bool forceKeyframe, std::vector<uint8_t>& out, bool* isKeyframe) = 0;

    proto::Codec codec() const { return codec_; }

    // VPS, SPS and PPS for the decoder (H.264 has no VPS).
    const std::vector<uint8_t>& codecConfig() const { return codecConfig_; }

protected:
    proto::Codec codec_ = proto::kCodecHevc;
    std::vector<uint8_t> codecConfig_;
};

// NVIDIA's own encoder, which encodes the capture texture as it is.
class NvencEncoder final : public VideoEncoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;
    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    bool open(ID3D11Device* device, ID3D11Texture2D* input, const Params& params) override;
    void close() override;
    bool encode(uint32_t frameId, bool forceKeyframe, std::vector<uint8_t>& out, bool* isKeyframe) override;

private:
    struct Impl;
    Impl* impl_;
};

// Whichever hardware encoder Windows offers for this GPU through Media Foundation: Intel Quick Sync and
// AMD's, or NVIDIA's where NVENC itself won't start. These want NV12, so it converts on the GPU first.
class MfEncoder final : public VideoEncoder {
public:
    explicit MfEncoder(proto::Codec codec);
    ~MfEncoder() override;
    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;

    bool open(ID3D11Device* device, ID3D11Texture2D* input, const Params& params) override;
    void close() override;
    bool encode(uint32_t frameId, bool forceKeyframe, std::vector<uint8_t>& out, bool* isKeyframe) override;

private:
    struct Impl;
    Impl* impl_;
};

enum class EncoderKind { Auto, Nvenc, MediaFoundation };
enum class CodecChoice { Auto, Hevc, H264 };

// NVENC on NVIDIA graphics, then Media Foundation, trying HEVC before H.264, which older hardware is limited to.
// Returns nothing if none of them will start on this GPU.
std::unique_ptr<VideoEncoder> openEncoder(ID3D11Device* device, ID3D11Texture2D* input, const VideoEncoder::Params& params, EncoderKind kind, CodecChoice codec);
