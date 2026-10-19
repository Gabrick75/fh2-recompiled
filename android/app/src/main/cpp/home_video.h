// The home screen's background video (MainActivity's home page): the game's own PressStart.wmv, read in place from
// the disc image or the extracted folder, demuxed here (ASF) and decoded with the libavcodec the game already links
// for its cutscenes (WMV3) and its sound (WMA Pro). No libavformat: only this one file has to be read.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace fh2 {

// One finished media object of a stream: a whole video frame or a whole audio block.
struct AsfPacket {
    int stream = 0;
    int64_t pts_ms = 0;
    bool keyframe = false;
    std::vector<uint8_t> data;
};

struct AsfStream {
    int number = -1;
    bool video = false;
    // Audio (WAVEFORMATEX)
    int format_tag = 0, channels = 0, sample_rate = 0, bit_rate = 0, block_align = 0, bits = 0;
    // Video (BITMAPINFOHEADER)
    int width = 0, height = 0;
    uint32_t fourcc = 0;
    std::vector<uint8_t> extradata;
};

// The file is the byte range [offset, offset + size) of path (the whole file, or one file inside the ISO).
class AsfReader {
public:
    bool Open(const std::string& path, int64_t offset, int64_t size);
    ~AsfReader();
    // The next finished media object, false at the end of the data (Rewind starts again).
    bool Next(AsfPacket& out);
    void Rewind();
    const AsfStream* video() const { return video_ >= 0 ? &streams_[video_] : nullptr; }
    const AsfStream* audio() const { return audio_ >= 0 ? &streams_[audio_] : nullptr; }
    int64_t preroll_ms() const { return preroll_ms_; }

private:
    bool Read(int64_t at, void* into, size_t bytes) const;
    bool ParseHeader();
    bool ParsePacket();

    int fd_ = -1;
    int64_t offset_ = 0, size_ = 0;
    int64_t data_start_ = 0, data_end_ = 0, next_packet_ = 0;
    uint32_t packet_size_ = 0;
    int64_t preroll_ms_ = 0;
    std::vector<AsfStream> streams_;
    int video_ = -1, audio_ = -1;
    // Media objects being put together, one per stream, and the finished ones not handed out yet.
    struct Partial {
        int stream = -1;
        uint32_t object = 0;
        uint32_t size = 0;
        int64_t pts_ms = 0;
        bool keyframe = false;
        std::vector<uint8_t> data;
    };
    std::vector<Partial> partial_;
    std::vector<AsfPacket> ready_;
    size_t ready_next_ = 0;
};

// Decodes the reader's two streams. Frames come out as yuv420p (video) and as stereo float (audio, the 5.1 folded
// down to two).
class HomeVideoDecoder {
public:
    using VideoSink = std::function<void(const AVFrame& frame, int64_t pts_ms)>;
    using AudioSink = std::function<void(const float* stereo, int frames, int64_t pts_ms)>;

    bool Open(const AsfReader& reader);
    ~HomeVideoDecoder();
    // decode_audio false skips the sound (muted): no work for it.
    void Decode(const AsfPacket& packet, bool decode_audio, const VideoSink& video, const AudioSink& audio);
    void Flush();
    int sample_rate() const { return audio_ ? audio_->sample_rate : 0; }

private:
    int video_stream_ = -1, audio_stream_ = -1;
    AVCodecContext* video_ = nullptr;
    AVCodecContext* audio_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::vector<float> stereo_;
};

}  // namespace fh2
