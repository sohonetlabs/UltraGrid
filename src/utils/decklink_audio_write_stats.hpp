/**
 * @file   utils/decklink_audio_write_stats.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_UTILS_DECKLINK_AUDIO_WRITE_STATS_HPP_8E2A4C17_3B9D_4F60_A5E1_7C0D2B6F9A34
#define SRC_UTILS_DECKLINK_AUDIO_WRITE_STATS_HPP_8E2A4C17_3B9D_4F60_A5E1_7C0D2B6F9A34

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>

#include "debug.h"
#include "utils/color_out.h"

/**
 * @brief Times each DeckLink audio write and the video calls made alongside it, to show where the
 *        audio thread spends a stall.
 *
 * The video_call_* methods are called from the display thread, the rest from the audio thread.
 */
class DecklinkAudioWriteStats {
public:
        using clock = std::chrono::steady_clock;

        static constexpr std::chrono::seconds REPORT_INTERVAL{10};
        /// About the drift fixer's target fill (2700 samples), so a longer gap can empty the card.
        static constexpr std::chrono::milliseconds SLOW_GAP{55};
        static constexpr uint64_t MAX_LOGGED_PER_REPORT = 24;

        void video_call_started() {
                this->video_calls_started++;
        }

        void video_call_ended(clock::duration took) {
                this->video_calls_ended++;
                int64_t took_us = std::chrono::duration_cast<std::chrono::microseconds>(took).count();
                int64_t prev = this->video_call_max_us.load();
                while (took_us > prev && !this->video_call_max_us.compare_exchange_weak(prev, took_us)) {
                }
        }

        /// The audio frame has reached the DeckLink module.
        void write_entered(clock::time_point now = clock::now()) {
                this->entered = now;
        }

        /// The overflow wait is over and the write call is about to be made.
        void write_call_started(clock::time_point now = clock::now()) {
                this->call_started = now;
                this->video_calls_ended_at_call = this->video_calls_ended.load();
        }

        /// @param buffered samples the card held before this write
        void write_finished(uint32_t buffered, clock::time_point now = clock::now()) {
                this->writes++;
                this->video_calls_during_write = this->video_calls_started.load() - this->video_calls_ended_at_call;

                auto wait = this->call_started - this->entered;
                auto call = now - this->call_started;
                this->wait_max = std::max(this->wait_max, wait);
                this->call_max = std::max(this->call_max, call);

                if (this->has_prev_write) {
                        auto gap = now - this->prev_finished;
                        auto outside = this->entered - this->prev_finished;
                        this->gap_max = std::max(this->gap_max, gap);
                        this->outside_max = std::max(this->outside_max, outside);
                        if (gap > SLOW_GAP) {
                                this->slow_writes++;
                                this->log_slow_write(gap, outside, wait, call, buffered);
                        }
                } else {
                        this->has_prev_write = true;
                        this->last_summary = now;
                }
                this->prev_finished = now;

                if (now - this->last_summary > REPORT_INTERVAL) {
                        this->report();
                        this->reset_period();
                        this->last_summary = now;
                }
        }

        uint64_t get_writes() const {
                return this->writes;
        }

        uint64_t get_slow_writes() const {
                return this->slow_writes;
        }

        uint64_t get_slow_writes_logged() const {
                return this->slow_writes_logged;
        }

        uint64_t get_video_calls_during_write() const {
                return this->video_calls_during_write;
        }

private:
        static double ms(clock::duration d) {
                return std::chrono::duration<double, std::milli>(d).count();
        }

        void log_slow_write(clock::duration gap, clock::duration outside, clock::duration wait,
                        clock::duration call, uint32_t buffered) {
                if (this->period_slow_writes++ >= MAX_LOGGED_PER_REPORT) {
                        return;
                }
                this->slow_writes_logged++;
                LOG(LOG_LEVEL_INFO) << std::fixed << std::setprecision(1)
                                << "Decklink audio write stats - Slow write: " << SBOLD(this->writes)
                                << " / Since last write (ms): " << SBOLD(ms(gap))
                                << " / Outside module (ms): " << SBOLD(ms(outside))
                                << " / Overflow wait (ms): " << SBOLD(ms(wait))
                                << " / Write call (ms): " << SBOLD(ms(call))
                                << " / Buffered: " << SBOLD(buffered)
                                << " / Video calls during write: " << SBOLD(this->video_calls_during_write)
                                << (this->period_slow_writes == MAX_LOGGED_PER_REPORT ? " / No more logged until the next report" : "")
                                << "\n";
        }

        void report() {
                LOG(LOG_LEVEL_INFO) << std::fixed << std::setprecision(1)
                                << SUNDERLINE("Decklink audio write stats (cumulative)")
                                << " - Total Writes: " << SBOLD(this->writes)
                                << " / Slow Writes: " << SBOLD(this->slow_writes)
                                << " / Max since last write (ms): " << SBOLD(ms(this->gap_max))
                                << " / Max outside module (ms): " << SBOLD(ms(this->outside_max))
                                << " / Max overflow wait (ms): " << SBOLD(ms(this->wait_max))
                                << " / Max write call (ms): " << SBOLD(ms(this->call_max))
                                << " / Max video call (ms): " << SBOLD(this->video_call_max_us.exchange(0) / 1000.0)
                                << "\n";
        }

        void reset_period() {
                this->gap_max = clock::duration::zero();
                this->outside_max = clock::duration::zero();
                this->wait_max = clock::duration::zero();
                this->call_max = clock::duration::zero();
                this->period_slow_writes = 0;
        }

        std::atomic<uint64_t> video_calls_started{0};
        std::atomic<uint64_t> video_calls_ended{0};
        std::atomic<int64_t> video_call_max_us{0};

        uint64_t writes = 0;
        uint64_t slow_writes = 0;
        uint64_t slow_writes_logged = 0;
        uint64_t video_calls_ended_at_call = 0;
        uint64_t video_calls_during_write = 0;

        bool has_prev_write = false;
        clock::time_point entered{};
        clock::time_point call_started{};
        clock::time_point prev_finished{};
        clock::time_point last_summary{};

        // max values and the logged count cover only the time since the last report
        clock::duration gap_max = clock::duration::zero();
        clock::duration outside_max = clock::duration::zero();
        clock::duration wait_max = clock::duration::zero();
        clock::duration call_max = clock::duration::zero();
        uint64_t period_slow_writes = 0;
};

#endif // defined SRC_UTILS_DECKLINK_AUDIO_WRITE_STATS_HPP_8E2A4C17_3B9D_4F60_A5E1_7C0D2B6F9A34
