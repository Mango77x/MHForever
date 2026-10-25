#include "media/avc_decoder.hpp"

#include <cstring>
#include <deque>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace mhf::psp::media {

struct AvcDecoder::Impl {
    AVCodecContext *context{};
    AVPacket *packet{};
    AVFrame *frame{};
    bool draining{};
    // Access units the decoder has not accepted yet. send_packet refuses a unit (EAGAIN) while it has a picture to hand
    // over, and a refused unit is not consumed: dropping it would break the reference frames and smear the rest of the
    // movie, so it waits here for the next call.
    std::deque<std::vector<std::uint8_t>> units;

    ~Impl() { close(); }

    void close() {
        avcodec_free_context(&context);
        av_packet_free(&packet);
        av_frame_free(&frame);
        units.clear();
        draining = false;
    }

    // Hands the queued units to the decoder for as long as it takes them.
    void send_queued() {
        while (!units.empty()) {
            const auto &unit = units.front();
            if (av_new_packet(packet, static_cast<int>(unit.size())) < 0) {
                units.pop_front();
                continue;
            }
            std::memcpy(packet->data, unit.data(), unit.size());
            const int sent = avcodec_send_packet(context, packet);
            av_packet_unref(packet);
            if (sent == AVERROR(EAGAIN)) return; // a picture is waiting: it goes out first
            units.pop_front();
            if (sent < 0) continue; // the unit itself is not decodable
        }
    }

    // Copies the picture FFmpeg holds into `picture`, dropping the row padding of every plane.
    bool take(Picture &picture) {
        if (avcodec_receive_frame(context, frame) < 0) return false;
        const auto format = static_cast<AVPixelFormat>(frame->format);
        if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P) {
            av_frame_unref(frame); // 4:2:2 or 4:4:4 would need a converter; the PSP's movies are 4:2:0
            return false;
        }
        picture.width = static_cast<std::uint32_t>(frame->width);
        picture.height = static_cast<std::uint32_t>(frame->height);
        picture.full_range = format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;
        const auto copy_plane = [&](int plane, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> &out) {
            out.resize(static_cast<std::size_t>(width) * height);
            for (std::uint32_t row = 0; row < height; ++row) {
                std::memcpy(out.data() + static_cast<std::size_t>(row) * width,
                            frame->data[plane] + static_cast<std::ptrdiff_t>(row) * frame->linesize[plane], width);
            }
        };
        copy_plane(0, picture.width, picture.height, picture.y);
        copy_plane(1, (picture.width + 1u) / 2u, (picture.height + 1u) / 2u, picture.cb);
        copy_plane(2, (picture.width + 1u) / 2u, (picture.height + 1u) / 2u, picture.cr);
        av_frame_unref(frame);
        return true;
    }
};

AvcDecoder::AvcDecoder() : impl_(std::make_unique<Impl>()) {}
AvcDecoder::~AvcDecoder() = default;


bool AvcDecoder::open() {
    Impl &impl = *impl_;
    impl.close();
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (decoder == nullptr) return false;
    impl.context = avcodec_alloc_context3(decoder);
    impl.packet = av_packet_alloc();
    impl.frame = av_frame_alloc();
    if (impl.context == nullptr || impl.packet == nullptr || impl.frame == nullptr) {
        impl.close();
        return false;
    }
    // One picture in, one picture out, in the calling thread: the guest expects each decode call to hand over its
    // picture, and the PSP decoder has no threading model to imitate. The low delay flag must stay off: it makes the
    // decoder hand pictures over before their references are finished, which smears the rest of the movie.
    impl.context->thread_count = 1;
    if (avcodec_open2(impl.context, decoder, nullptr) < 0) {
        impl.close();
        return false;
    }
    return true;
}

void AvcDecoder::close() { impl_->close(); }

bool AvcDecoder::is_open() const noexcept { return impl_->context != nullptr; }

bool AvcDecoder::decode(std::span<const std::uint8_t> unit, Picture &picture) {
    Impl &impl = *impl_;
    if (impl.context == nullptr || unit.empty()) return false;
    impl.draining = false;
    impl.units.emplace_back(unit.begin(), unit.end());
    impl.send_queued();
    // At most one picture per call: the guest reads one frame buffer per decode.
    return impl.take(picture);
}

bool AvcDecoder::drain(Picture &picture) {
    Impl &impl = *impl_;
    if (impl.context == nullptr) return false;
    if (!impl.draining) {
        impl.draining = true;
        impl.units.clear();
        avcodec_send_packet(impl.context, nullptr); // flush: the pictures the decoder still holds come out
    }
    return impl.take(picture);
}

} // namespace mhf::psp::media
