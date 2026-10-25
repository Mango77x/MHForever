#include "media/atrac_decoder.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}

namespace mhf::psp::media {

AtracDecoder::~AtracDecoder() { close(); }

bool AtracDecoder::open(unsigned channels, unsigned bytes_per_frame) {
    close();
    if (channels == 0 || channels > 2 || bytes_per_frame == 0) return false;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (codec == nullptr) return false;
    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) return false;
    context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    // The PSMF header does not say the sample rate; the PSP sound library uses the same one for every movie.
    context_->sample_rate = 44100;
    context_->block_align = static_cast<int>(bytes_per_frame);
    av_channel_layout_default(&context_->ch_layout, static_cast<int>(channels));
    if (avcodec_open2(context_, codec, nullptr) < 0) {
        close();
        return false;
    }
    channels_ = channels;
    return true;
}

void AtracDecoder::close() {
    if (context_ != nullptr) avcodec_free_context(&context_);
    context_ = nullptr;
}

bool AtracDecoder::decode(std::span<const std::uint8_t> frame, std::int16_t *out) noexcept {
    std::memset(out, 0, static_cast<std::size_t>(kAtracSamplesPerFrame) * channels_ * sizeof(std::int16_t));
    if (context_ == nullptr || frame.empty()) return false;
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();
    if (packet == nullptr || decoded == nullptr) {
        av_packet_free(&packet);
        av_frame_free(&decoded);
        return false;
    }
    bool produced = false;
    if (av_new_packet(packet, static_cast<int>(frame.size())) >= 0) {
        std::memcpy(packet->data, frame.data(), frame.size());
        if (avcodec_send_packet(context_, packet) >= 0) {
            while (avcodec_receive_frame(context_, decoded) == 0) {
                if (decoded->format == AV_SAMPLE_FMT_FLTP && decoded->nb_samples > 0) {
                    const int count = std::min<int>(decoded->nb_samples, kAtracSamplesPerFrame);
                    const float *left = reinterpret_cast<const float *>(decoded->data[0]);
                    const float *right = channels_ == 2 ? reinterpret_cast<const float *>(decoded->data[1]) : left;
                    for (int i = 0; i < count; ++i) {
                        out[static_cast<std::size_t>(i) * channels_] = static_cast<std::int16_t>(
                            std::clamp(left[i], -1.0f, 1.0f) * 32767.0f);
                        if (channels_ == 2)
                            out[static_cast<std::size_t>(i) * 2u + 1u] = static_cast<std::int16_t>(
                                std::clamp(right[i], -1.0f, 1.0f) * 32767.0f);
                    }
                    produced = count > 0;
                }
                av_frame_unref(decoded);
                break; // one frame in, one frame out
            }
        }
    }
    av_frame_free(&decoded);
    av_packet_free(&packet);
    return produced;
}

} // namespace mhf::psp::media
