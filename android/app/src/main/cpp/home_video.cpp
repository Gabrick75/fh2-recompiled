// The home screen video's demuxer and decoder (home_video.h). ASF as in Microsoft's "Advanced Systems Format
// Specification" rev 01.20.03: a header object (file and stream properties), then a data object of fixed-size data
// packets whose payloads are pieces of the streams' media objects.
#include "home_video.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace fh2 {
namespace {

using Guid = uint8_t[16];
// The GUIDs as stored in the file (the first three fields little endian).
const Guid kHeader = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
const Guid kData = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
const Guid kFileProperties = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                              0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
const Guid kStreamProperties = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
const Guid kAudioMedia = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
const Guid kVideoMedia = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32; }

// A field whose width is given by a 2-bit length type: 0 absent, 1 byte, 2 word, 3 dword.
bool Field(const uint8_t*& p, const uint8_t* end, int type, uint32_t& value) {
    static const int kWidth[4] = {0, 1, 2, 4};
    int width = kWidth[type & 3];
    if (end - p < width) return false;
    value = width == 0 ? 0 : width == 1 ? p[0] : width == 2 ? U16(p) : U32(p);
    p += width;
    return true;
}

}  // namespace

AsfReader::~AsfReader() {
    if (fd_ >= 0) close(fd_);
}

bool AsfReader::Read(int64_t at, void* into, size_t bytes) const {
    if (at < 0 || at + int64_t(bytes) > size_) return false;
    auto* out = static_cast<uint8_t*>(into);
    while (bytes) {
        ssize_t got = pread(fd_, out, bytes, offset_ + at);
        if (got <= 0) return false;
        out += got;
        at += got;
        bytes -= size_t(got);
    }
    return true;
}

bool AsfReader::Open(const std::string& path, int64_t offset, int64_t size) {
    fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    offset_ = offset;
    size_ = size;
    return ParseHeader() && video_ >= 0;
}

bool AsfReader::ParseHeader() {
    uint8_t top[30];
    if (!Read(0, top, sizeof(top)) || memcmp(top, kHeader, 16) != 0) return false;
    uint64_t header_size = U64(top + 16);
    if (header_size < 30 || header_size > 1 << 20) return false;
    std::vector<uint8_t> header(header_size);
    if (!Read(0, header.data(), header.size())) return false;

    for (size_t at = 30; at + 24 <= header.size();) {
        const uint8_t* object = header.data() + at;
        uint64_t object_size = U64(object + 16);
        if (object_size < 24 || at + object_size > header.size()) return false;
        if (memcmp(object, kFileProperties, 16) == 0 && object_size >= 104) {
            preroll_ms_ = int64_t(U64(object + 80));
            packet_size_ = U32(object + 92);  // the minimum and maximum packet sizes are the same
        } else if (memcmp(object, kStreamProperties, 16) == 0 && object_size >= 78) {
            AsfStream stream;
            stream.number = U16(object + 72) & 0x7F;
            uint32_t specific = U32(object + 64);
            const uint8_t* data = object + 78;
            if (78 + specific > object_size) return false;
            if (memcmp(object + 24, kAudioMedia, 16) == 0 && specific >= 18) {
                stream.format_tag = U16(data);
                stream.channels = U16(data + 2);
                stream.sample_rate = int(U32(data + 4));
                stream.bit_rate = int(U32(data + 8)) * 8;
                stream.block_align = U16(data + 12);
                stream.bits = U16(data + 14);
                uint16_t extra = std::min<uint32_t>(U16(data + 16), specific - 18);
                stream.extradata.assign(data + 18, data + 18 + extra);
                if (audio_ < 0) audio_ = int(streams_.size());
                streams_.push_back(std::move(stream));
            } else if (memcmp(object + 24, kVideoMedia, 16) == 0 && specific >= 11 + 40) {
                const uint8_t* bitmap = data + 11;
                uint32_t bitmap_size = std::min<uint32_t>(U32(bitmap), specific - 11);
                stream.video = true;
                stream.width = int(U32(bitmap + 4));
                stream.height = int(U32(bitmap + 8));
                stream.fourcc = U32(bitmap + 16);
                if (bitmap_size > 40) stream.extradata.assign(bitmap + 40, bitmap + bitmap_size);
                if (video_ < 0) video_ = int(streams_.size());
                streams_.push_back(std::move(stream));
            }
        }
        at += object_size;
    }
    if (packet_size_ == 0) return false;

    uint8_t data[50];
    if (!Read(int64_t(header_size), data, sizeof(data)) || memcmp(data, kData, 16) != 0) return false;
    data_start_ = int64_t(header_size) + 50;
    uint64_t data_size = U64(data + 16);
    data_end_ = std::min<int64_t>(size_, int64_t(header_size) + int64_t(data_size));
    if (data_size == 0) data_end_ = size_;  // still being written when the size was stored
    Rewind();
    return true;
}

void AsfReader::Rewind() {
    next_packet_ = data_start_;
    partial_.clear();
    ready_.clear();
    ready_next_ = 0;
}

bool AsfReader::Next(AsfPacket& out) {
    while (ready_next_ >= ready_.size()) {
        ready_.clear();
        ready_next_ = 0;
        if (next_packet_ + packet_size_ > data_end_) return false;
        ParsePacket();  // a damaged packet is skipped
        next_packet_ += packet_size_;
    }
    out = std::move(ready_[ready_next_++]);
    return true;
}

bool AsfReader::ParsePacket() {
    std::vector<uint8_t> packet(packet_size_);
    if (!Read(next_packet_, packet.data(), packet.size())) return false;
    const uint8_t* p = packet.data();
    const uint8_t* end = p + packet.size();

    if (*p & 0x80) {  // error correction data: its length in the low 4 bits
        int length = *p & 0x0F;
        p += 1 + length;
    }
    if (end - p < 2) return false;
    uint8_t flags = *p++;
    uint8_t properties = *p++;
    uint32_t packet_length = 0, sequence = 0, padding = 0;
    if (!Field(p, end, flags >> 5, packet_length) || !Field(p, end, flags >> 1, sequence) ||
        !Field(p, end, flags >> 3, padding))
        return false;
    if (end - p < 6) return false;
    p += 6;  // send time, duration
    if (packet_length == 0 || packet_length > packet_size_) packet_length = packet_size_;
    const uint8_t* payloads_end = packet.data() + packet_length - std::min(padding, packet_length);
    if (payloads_end > end) payloads_end = end;

    bool multiple = flags & 1;
    int count = 1, length_type = 2;
    if (multiple) {
        if (p >= payloads_end) return false;
        count = *p & 0x3F;
        length_type = *p >> 6;
        p++;
    }
    for (int i = 0; i < count; i++) {
        if (p >= payloads_end) return false;
        int stream = *p & 0x7F;
        bool keyframe = *p & 0x80;
        p++;
        uint32_t object = 0, offset = 0, replicated = 0;
        if (!Field(p, payloads_end, properties >> 4, object) || !Field(p, payloads_end, properties >> 2, offset) ||
            !Field(p, payloads_end, properties, replicated))
            return false;
        const uint8_t* replicated_data = p;
        if (replicated != 1) {
            if (payloads_end - p < int64_t(replicated)) return false;
            p += replicated;
        } else {
            if (p >= payloads_end) return false;
            p++;  // the presentation time delta of the compressed payload's sub-payloads
        }
        uint32_t length = 0;
        if (multiple) {
            if (!Field(p, payloads_end, length_type, length)) return false;
        } else {
            length = uint32_t(payloads_end - p);
        }
        if (payloads_end - p < int64_t(length)) return false;
        const uint8_t* data = p;
        p += length;

        bool known = false;
        for (const AsfStream& s : streams_) known |= s.number == stream;
        if (!known) continue;

        if (replicated == 1) {
            // Compressed payload: whole media objects, each after its one-byte length; the offset field holds the
            // presentation time.
            for (const uint8_t* sub = data; sub < data + length;) {
                uint32_t sub_length = *sub++;
                if (data + length - sub < int64_t(sub_length)) break;
                AsfPacket finished;
                finished.stream = stream;
                finished.pts_ms = int64_t(offset) - preroll_ms_;
                finished.keyframe = keyframe;
                finished.data.assign(sub, sub + sub_length);
                ready_.push_back(std::move(finished));
                sub += sub_length;
            }
            continue;
        }
        if (replicated < 8) continue;
        uint32_t object_size = U32(replicated_data);
        int64_t pts = int64_t(U32(replicated_data + 4)) - preroll_ms_;
        if (object_size == 0 || object_size > 16 << 20) continue;

        Partial* building = nullptr;
        for (Partial& part : partial_)
            if (part.stream == stream) building = &part;
        if (!building) {
            partial_.push_back(Partial{});
            building = &partial_.back();
            building->stream = stream;
        }
        if (offset == 0) {  // a new object (an unfinished one before it is dropped)
            building->object = object;
            building->size = object_size;
            building->pts_ms = pts;
            building->keyframe = keyframe;
            building->data.clear();
            building->data.reserve(object_size);
        } else if (building->object != object || building->data.size() != offset || building->size != object_size) {
            building->data.clear();
            building->size = 0;
            continue;  // a piece lost before this one
        }
        if (building->size == 0) continue;
        uint32_t take = std::min<uint32_t>(length, building->size - uint32_t(building->data.size()));
        building->data.insert(building->data.end(), data, data + take);
        if (building->data.size() == building->size) {
            AsfPacket finished;
            finished.stream = stream;
            finished.pts_ms = building->pts_ms;
            finished.keyframe = building->keyframe;
            finished.data = std::move(building->data);
            ready_.push_back(std::move(finished));
            building->data = {};
            building->size = 0;
        }
    }
    return true;
}

// ---- Decoder ---------------------------------------------------------------------------------------------------

namespace {

AVCodecContext* OpenCodec(AVCodecID id, const AsfStream& stream) {
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) return nullptr;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) return nullptr;
    if (!stream.extradata.empty()) {
        context->extradata = static_cast<uint8_t*>(
                av_mallocz(stream.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(context->extradata, stream.extradata.data(), stream.extradata.size());
        context->extradata_size = int(stream.extradata.size());
    }
    if (stream.video) {
        context->width = stream.width;
        context->height = stream.height;
        context->codec_tag = stream.fourcc;
        context->thread_count = 2;  // slices of a 720p frame: a big core is enough, two keep it far from late
    } else {
        context->channels = stream.channels;
        context->sample_rate = stream.sample_rate;
        context->bit_rate = stream.bit_rate;
        context->block_align = stream.block_align;
        context->bits_per_coded_sample = stream.bits;
    }
    context->pkt_timebase = AVRational{1, 1000};
    if (avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

}  // namespace

bool HomeVideoDecoder::Open(const AsfReader& reader) {
    const AsfStream* video = reader.video();
    if (!video || video->fourcc != MKTAG('W', 'M', 'V', '3')) return false;
    video_stream_ = video->number;
    video_ = OpenCodec(AV_CODEC_ID_WMV3, *video);
    if (!video_) return false;
    const AsfStream* audio = reader.audio();
    if (audio && audio->format_tag == 0x0162) {  // WMA Pro; without it the video plays silent
        audio_ = OpenCodec(AV_CODEC_ID_WMAPRO, *audio);
        if (audio_) audio_stream_ = audio->number;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ && packet_;
}

HomeVideoDecoder::~HomeVideoDecoder() {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&video_);
    avcodec_free_context(&audio_);
}

void HomeVideoDecoder::Flush() {
    if (video_) avcodec_flush_buffers(video_);
    if (audio_) avcodec_flush_buffers(audio_);
}

void HomeVideoDecoder::Decode(const AsfPacket& in, bool decode_audio, const VideoSink& video,
                              const AudioSink& audio) {
    bool is_video = in.stream == video_stream_;
    bool is_audio = audio_ && in.stream == audio_stream_;
    if (!is_video && !(is_audio && decode_audio)) return;
    AVCodecContext* context = is_video ? video_ : audio_;

    // An audio object may hold several of the codec's blocks: one packet each.
    size_t step = in.data.size();
    if (is_audio && context->block_align > 0 && in.data.size() % context->block_align == 0)
        step = size_t(context->block_align);
    // The sound's blocks after the object's first carry no timestamp: each follows the samples before it.
    int64_t audio_samples = 0;
    for (size_t at = 0; at < in.data.size(); at += step) {
        size_t bytes = std::min(step, in.data.size() - at);
        if (av_new_packet(packet_, int(bytes)) < 0) return;
        memcpy(packet_->data, in.data.data() + at, bytes);
        packet_->pts = at == 0 ? in.pts_ms : AV_NOPTS_VALUE;
        packet_->dts = packet_->pts;
        if (in.keyframe) packet_->flags |= AV_PKT_FLAG_KEY;
        int sent = avcodec_send_packet(context, packet_);
        av_packet_unref(packet_);
        if (sent < 0 && sent != AVERROR(EAGAIN)) continue;  // a damaged block: the next one decodes

        while (avcodec_receive_frame(context, frame_) == 0) {
            int64_t pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp
                                                                           : in.pts_ms;
            if (!is_video && context->sample_rate > 0) {
                pts = in.pts_ms + audio_samples * 1000 / context->sample_rate;
                audio_samples += frame_->nb_samples;
            }
            if (is_video) {
                video(*frame_, pts);
            } else {
                // Stereo from the decoder's planar float: 5.1 (FL FR FC LFE BL BR) folds the center and the backs
                // into the fronts, the LFE left out; mono is doubled. The fold is scaled so a front and its back
                // at full scale do not clip (the mix peaks at about a quarter of full scale).
                int frames = frame_->nb_samples, channels = frame_->channels;
                stereo_.resize(size_t(frames) * 2);
                const float* const* planes = reinterpret_cast<const float* const*>(frame_->extended_data);
                if (frame_->format == AV_SAMPLE_FMT_FLTP && channels >= 1) {
                    for (int i = 0; i < frames; i++) {
                        float left, right;
                        if (channels == 1) {
                            left = right = planes[0][i];
                        } else if (channels < 6) {
                            left = planes[0][i];
                            right = planes[1][i];
                        } else {
                            const float k = 0.7071f, scale = 1.0f / (1.0f + k);
                            left = (planes[0][i] + k * planes[2][i] + k * planes[4][i]) * scale;
                            right = (planes[1][i] + k * planes[2][i] + k * planes[5][i]) * scale;
                        }
                        stereo_[size_t(i) * 2] = left;
                        stereo_[size_t(i) * 2 + 1] = right;
                    }
                    audio(stereo_.data(), frames, pts);
                }
            }
            av_frame_unref(frame_);
        }
    }
}

}  // namespace fh2
