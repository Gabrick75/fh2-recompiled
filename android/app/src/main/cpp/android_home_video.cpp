// The home screen's background video on Android (HomeVideo.java): home_video.h reads and decodes, this plays.
//
// In the file the sound runs 2.3 to 3.5 s behind the picture, so one thread pacing on the picture would bring the
// sound late. Two threads instead: the reader decodes the sound at once into a ring and queues the picture still
// compressed (a few MB for 5 s); the picture thread decodes it and shows each frame at its time. Both share one
// timeline, in ms from the start: each loop adds the clip's length, and every sound block is put in the ring at the
// sample its timestamp says (silence before it, or its late part dropped), which keeps the two together across loops
// and when the sound is turned on mid-clip.
#include "home_video.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "FH2", __VA_ARGS__)

namespace fh2 {
namespace {

using Clock = std::chrono::steady_clock;

class HomeVideoPlayer {
public:
    bool Open(const std::string& path, int64_t offset, int64_t size) {
        path_ = path;
        offset_ = offset;
        size_ = size;
        AsfReader probe;
        HomeVideoDecoder decoder;
        return probe.Open(path, offset, size) && decoder.Open(probe);
    }

    ~HomeVideoPlayer() { Stop(); }

    void Start(ANativeWindow* window, bool sound) {
        Stop();
        window_ = window;
        sound_ = sound;
        running_ = true;
        reader_ = std::make_unique<AsfReader>();
        decoder_ = std::make_unique<HomeVideoDecoder>();
        video_decoder_ = std::make_unique<HomeVideoDecoder>();
        if (!reader_->Open(path_, offset_, size_) || !decoder_->Open(*reader_) || !video_decoder_->Open(*reader_)) {
            running_ = false;
            return;
        }
        const AsfStream* video = reader_->video();
        ANativeWindow_setBuffersGeometry(window_, video->width, video->height, WINDOW_FORMAT_RGBX_8888);
        sample_rate_ = decoder_->sample_rate();
        if (sample_rate_ > 0) OpenAudio();
        start_ = Clock::now();
        reader_thread_ = std::thread([this] { ReadLoop(); });
        video_thread_ = std::thread([this] { VideoLoop(); });
    }

    void Stop() {
        running_ = false;
        queue_ready_.notify_all();
        queue_room_.notify_all();
        if (reader_thread_.joinable()) reader_thread_.join();
        if (video_thread_.joinable()) video_thread_.join();
        if (audio_) {
            AAudioStream_requestStop(audio_);
            AAudioStream_close(audio_);
            audio_ = nullptr;
        }
        if (window_) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
        queue_.clear();
        video_decoder_.reset();
        decoder_.reset();
        reader_.reset();
    }

    void SetSound(bool sound) { sound_ = sound; }

private:
    static constexpr int64_t kQueueMs = 5000;    // compressed picture read ahead (the sound is up to 3.5 s behind)
    static constexpr int kRingSeconds = 8;       // decoded sound ahead of the speaker
    static constexpr int64_t kFrameMs = 33;      // 29.97 fps
    static constexpr int64_t kResyncMs = 150;    // how far the sound may drift before it is put back in place

    int64_t NowMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count();
    }

    // ---- Reader: the sound into the ring, the picture into the queue -------------------------------------------

    void ReadLoop() {
        int64_t epoch = 0, last_video = 0;
        AsfPacket packet;
        while (running_) {
            if (!reader_->Next(packet)) {  // the end: again from the start, later on the timeline
                epoch += last_video + kFrameMs;
                last_video = 0;
                reader_->Rewind();
                decoder_->Flush();
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(Queued{true, {}});  // the picture decoder starts over too
                continue;
            }
            packet.pts_ms += epoch;
            if (packet.stream == reader_->video()->number) {
                last_video = std::max(last_video, packet.pts_ms - epoch);
                std::unique_lock lock(queue_mutex_);
                queue_room_.wait(lock, [&] {
                    return !running_ || queue_.empty() || packet.pts_ms - NowMs() < kQueueMs;
                });
                if (!running_) break;
                queue_.push_back(Queued{false, std::move(packet)});
                queue_ready_.notify_one();
            } else if (audio_ && sound_) {
                decoder_->Decode(packet, true, [](const AVFrame&, int64_t) {},
                                [&](const float* stereo, int frames, int64_t pts) { PushSound(stereo, frames, pts); });
            }
        }
    }

    // The sound goes on right after what was written; only a block more than kResyncMs off (the sound was off, a
    // block was lost, the speaker ran dry) moves it: silence up to it, or its late part dropped.
    void PushSound(const float* stereo, int frames, int64_t pts_ms) {
        int64_t target = pts_ms * sample_rate_ / 1000;
        int64_t written = std::max(written_.load(), read_.load());
        int64_t tolerance = int64_t(sample_rate_) * kResyncMs / 1000;
        int skip = 0;
        if (target > written + tolerance) {
            int64_t gap = std::min<int64_t>(target - written, ring_frames_ / 2);
            for (int64_t i = 0; i < gap && WaitRoom(written); i++, written++) {
                ring_[(written % ring_frames_) * 2] = 0;
                ring_[(written % ring_frames_) * 2 + 1] = 0;
            }
        } else if (target + tolerance < written) {
            skip = int(std::min<int64_t>(frames, written - target));
        }
        for (int i = skip; i < frames && WaitRoom(written); i++, written++) {
            ring_[(written % ring_frames_) * 2] = stereo[i * 2];
            ring_[(written % ring_frames_) * 2 + 1] = stereo[i * 2 + 1];
            if ((i & 1023) == 1023) written_ = written;  // the speaker can take it meanwhile
        }
        written_ = written;
    }

    bool WaitRoom(int64_t written) {
        while (running_ && written - read_.load() >= ring_frames_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return running_;
    }

    // ---- Sound out --------------------------------------------------------------------------------------------

    void OpenAudio() {
        ring_frames_ = int64_t(sample_rate_) * kRingSeconds;
        ring_.assign(size_t(ring_frames_) * 2, 0.0f);
        read_ = 0;
        written_ = 0;
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return;
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, sample_rate_);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
        AAudioStreamBuilder_setDataCallback(builder, &HomeVideoPlayer::SoundCallback, this);
        if (AAudioStreamBuilder_openStream(builder, &audio_) != AAUDIO_OK) audio_ = nullptr;
        AAudioStreamBuilder_delete(builder);
        if (audio_ && AAudioStream_requestStart(audio_) != AAUDIO_OK) {
            AAudioStream_close(audio_);
            audio_ = nullptr;
        }
        if (!audio_) LOG("home video: no sound output");
    }

    // The speaker's side: always moves on with the clock; silence where nothing was written or with the sound off.
    static aaudio_data_callback_result_t SoundCallback(AAudioStream*, void* user, void* data, int32_t frames) {
        auto* self = static_cast<HomeVideoPlayer*>(user);
        auto* out = static_cast<float*>(data);
        int64_t read = self->read_.load(), written = self->written_.load();
        bool sound = self->sound_.load();
        for (int32_t i = 0; i < frames; i++, read++) {
            bool have = read < written && sound;
            out[i * 2] = have ? self->ring_[(read % self->ring_frames_) * 2] : 0.0f;
            out[i * 2 + 1] = have ? self->ring_[(read % self->ring_frames_) * 2 + 1] : 0.0f;
        }
        self->read_ = read;
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // ---- Picture ----------------------------------------------------------------------------------------------

    void VideoLoop() {
        while (running_) {
            Queued next;
            {
                std::unique_lock lock(queue_mutex_);
                queue_ready_.wait(lock, [&] { return !running_ || !queue_.empty(); });
                if (!running_) break;
                next = std::move(queue_.front());
                queue_.pop_front();
            }
            queue_room_.notify_one();
            if (next.restart) {
                video_decoder_->Flush();
                continue;
            }
            video_decoder_->Decode(next.packet, false,
                                   [&](const AVFrame& frame, int64_t pts) { Show(frame, pts); },
                                   [](const float*, int, int64_t) {});
        }
    }

    void Show(const AVFrame& frame, int64_t pts_ms) {
        int64_t wait = pts_ms - NowMs();
        if (wait < -100) return;  // late (the phone was busy): the next frame catches up
        while (running_ && wait > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(wait, 50)));
            wait = pts_ms - NowMs();
        }
        if (!running_ || frame.format != AV_PIX_FMT_YUV420P) return;
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) return;
        int width = std::min(frame.width, buffer.width), height = std::min(frame.height, buffer.height);
        for (int y = 0; y < height; y++) {
            const uint8_t* luma = frame.data[0] + y * frame.linesize[0];
            const uint8_t* cb = frame.data[1] + (y / 2) * frame.linesize[1];
            const uint8_t* cr = frame.data[2] + (y / 2) * frame.linesize[2];
            auto* out = static_cast<uint32_t*>(buffer.bits) + size_t(y) * buffer.stride;
            for (int x = 0; x < width; x++) {
                // BT.601, video range, 16.16 fixed point
                int c = (luma[x] - 16) * 76309, u = cb[x / 2] - 128, v = cr[x / 2] - 128;
                int r = (c + 104597 * v) >> 16;
                int g = (c - 25675 * u - 53279 * v) >> 16;
                int b = (c + 132201 * u) >> 16;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                out[x] = 0xFF000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | uint32_t(r);
            }
        }
        ANativeWindow_unlockAndPost(window_);
    }

    struct Queued {
        bool restart = false;
        AsfPacket packet;
    };

    std::string path_;
    int64_t offset_ = 0, size_ = 0;
    std::unique_ptr<AsfReader> reader_;
    std::unique_ptr<HomeVideoDecoder> decoder_;        // the sound, on the reader thread
    std::unique_ptr<HomeVideoDecoder> video_decoder_;  // the picture, on its own thread
    ANativeWindow* window_ = nullptr;
    AAudioStream* audio_ = nullptr;
    std::atomic<bool> running_{false}, sound_{false};
    Clock::time_point start_;
    std::thread reader_thread_, video_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_ready_, queue_room_;
    std::deque<Queued> queue_;
    int sample_rate_ = 0;
    std::vector<float> ring_;
    int64_t ring_frames_ = 1;
    std::atomic<int64_t> read_{0}, written_{0};  // samples on the timeline: the speaker's and the reader's
};

}  // namespace
}  // namespace fh2

extern "C" {

JNIEXPORT jlong JNICALL Java_com_forzahorizon2_android_HomeVideo_nativeOpen(JNIEnv* env, jclass, jstring path,
                                                                            jlong offset, jlong size) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    auto* player = new fh2::HomeVideoPlayer();
    bool opened = player->Open(chars, offset, size);
    env->ReleaseStringUTFChars(path, chars);
    if (!opened) {
        LOG("home video: cannot play it");
        delete player;
        return 0;
    }
    return reinterpret_cast<jlong>(player);
}

JNIEXPORT void JNICALL Java_com_forzahorizon2_android_HomeVideo_nativeStart(JNIEnv* env, jclass, jlong handle,
                                                                            jobject surface, jboolean sound) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window) reinterpret_cast<fh2::HomeVideoPlayer*>(handle)->Start(window, sound);
}

JNIEXPORT void JNICALL Java_com_forzahorizon2_android_HomeVideo_nativeStop(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<fh2::HomeVideoPlayer*>(handle)->Stop();
}

JNIEXPORT void JNICALL Java_com_forzahorizon2_android_HomeVideo_nativeSetSound(JNIEnv*, jclass, jlong handle,
                                                                               jboolean sound) {
    reinterpret_cast<fh2::HomeVideoPlayer*>(handle)->SetSound(sound);
}

JNIEXPORT void JNICALL Java_com_forzahorizon2_android_HomeVideo_nativeClose(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<fh2::HomeVideoPlayer*>(handle);
}

}  // extern "C"
