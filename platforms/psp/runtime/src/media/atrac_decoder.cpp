#include "media/atrac_decoder.hpp"

#include "at3_decoders.h"

#include <algorithm>
#include <cstring>

namespace mhf::psp::media {
namespace {

std::int16_t to_pcm(float value) noexcept {
    if (value >= 1.0f) return 32767;
    if (value <= -1.0f) return -32767;
    return static_cast<std::int16_t>(value * 32767.0f);
}

} // namespace

AtracDecoder::~AtracDecoder() { close(); }

bool AtracDecoder::open(unsigned channels, unsigned bytes_per_frame) {
    close();
    if (channels == 0 || channels > 2 || bytes_per_frame == 0) return false;
    int block_align = static_cast<int>(bytes_per_frame);
    context_ = atrac3p_alloc(static_cast<int>(channels), &block_align);
    channels_ = channels;
    return context_ != nullptr;
}

void AtracDecoder::close() {
    if (context_ != nullptr) atrac3p_free(context_);
    context_ = nullptr;
}

bool AtracDecoder::decode(std::span<const std::uint8_t> frame, std::int16_t *out) noexcept {
    std::memset(out, 0, static_cast<std::size_t>(kAtracSamplesPerFrame) * channels_ * sizeof(std::int16_t));
    if (context_ == nullptr || frame.empty()) return false;
    float *planes[2] = {left_.data(), right_.data()};
    int produced = 0;
    if (atrac3p_decode_frame(context_, planes, &produced, frame.data(), static_cast<int>(frame.size())) < 0) return false;
    produced = std::clamp(produced, 0, static_cast<int>(kAtracSamplesPerFrame));
    const float *right = channels_ == 2 ? right_.data() : left_.data();
    for (int i = 0; i < produced; ++i) {
        out[static_cast<std::size_t>(i) * channels_] = to_pcm(left_[static_cast<std::size_t>(i)]);
        if (channels_ == 2) out[static_cast<std::size_t>(i) * 2u + 1u] = to_pcm(right[i]);
    }
    return produced > 0;
}

} // namespace mhf::psp::media
