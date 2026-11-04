/**
 * @file   utils/audio_write_queue.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_UTILS_AUDIO_WRITE_QUEUE_HPP_6C1E8A35_2F4B_4D97_8E0A_B3D5C7F91A24
#define SRC_UTILS_AUDIO_WRITE_QUEUE_HPP_6C1E8A35_2F4B_4D97_8E0A_B3D5C7F91A24

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "utils/thread.h"

/**
 * @brief Hands audio frames to a writer on its own thread, so a writer that waits on the device
 *        doesn't hold up the thread that receives the audio.
 */
class AudioWriteQueue {
public:
        struct Frame {
                std::vector<char> data;
                int bps = 0;
                int ch_count = 0;

                size_t samples() const {
                        return this->data.size() / (this->bps * this->ch_count);
                }
        };
        using Writer = std::function<void(const Frame &)>;

        AudioWriteQueue(Writer writer, size_t max_frames)
                : writer(std::move(writer)), max_frames(max_frames), thread(&AudioWriteQueue::run, this) {
        }

        ~AudioWriteQueue() {
                {
                        std::lock_guard<std::mutex> lk(this->lock);
                        this->stopping = true;
                }
                this->frame_ready.notify_one();
                this->thread.join();
        }

        AudioWriteQueue(const AudioWriteQueue &) = delete;
        AudioWriteQueue &operator=(const AudioWriteQueue &) = delete;

        /// Copies the frame for the writer. Never blocks on the writer.
        /// @retval false the queue was full, so its oldest frame was dropped to make room
        bool push(const char *data, size_t len, int bps, int ch_count) {
                bool dropped = false;
                {
                        std::lock_guard<std::mutex> lk(this->lock);
                        if (this->frames.size() >= this->max_frames) {
                                this->frames.pop_front();
                                this->dropped_frames++;
                                dropped = true;
                        }
                        this->frames.push_back({std::vector<char>(data, data + len), bps, ch_count});
                }
                this->frame_ready.notify_one();
                return !dropped;
        }

        /// Drops frames not yet written, such as ones in a format the device no longer expects.
        void clear() {
                std::lock_guard<std::mutex> lk(this->lock);
                this->frames.clear();
        }

        /// Stops writing. Frames keep queuing, the oldest dropped once the queue is full.
        void hold() {
                std::lock_guard<std::mutex> lk(this->lock);
                this->held = true;
        }

        /// Resumes writing, first dropping the oldest frames until no more than `max_samples` remain.
        /// @returns the samples dropped
        size_t release(size_t max_samples) {
                size_t dropped_samples = 0;
                {
                        std::lock_guard<std::mutex> lk(this->lock);
                        size_t queued = 0;
                        for (const auto &frame : this->frames) {
                                queued += frame.samples();
                        }
                        while (queued > max_samples) {
                                queued -= this->frames.front().samples();
                                dropped_samples += this->frames.front().samples();
                                this->frames.pop_front();
                        }
                        this->held = false;
                }
                this->frame_ready.notify_one();
                return dropped_samples;
        }

        bool is_held() const {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->held;
        }

        uint64_t get_dropped_frames() const {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->dropped_frames;
        }

private:
        void run() {
                set_thread_name("audio_writer");
                std::unique_lock<std::mutex> lk(this->lock);
                while (true) {
                        this->frame_ready.wait(lk, [this] { return this->stopping || (!this->held && !this->frames.empty()); });
                        if (this->stopping) {
                                return;
                        }
                        Frame frame = std::move(this->frames.front());
                        this->frames.pop_front();
                        lk.unlock();
                        this->writer(frame);
                        lk.lock();
                }
        }

        Writer writer;
        size_t max_frames;
        mutable std::mutex lock;
        std::condition_variable frame_ready;
        std::deque<Frame> frames;
        uint64_t dropped_frames = 0;
        bool held = false;
        bool stopping = false;
        std::thread thread;
};

#endif // defined SRC_UTILS_AUDIO_WRITE_QUEUE_HPP_6C1E8A35_2F4B_4D97_8E0A_B3D5C7F91A24
