/**
 * @file   utils/frame_interval_stats.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_UTILS_FRAME_INTERVAL_STATS_HPP_5B0E3C8E_7D5A_4B7E_9F0B_2C4E1B6A9D13
#define SRC_UTILS_FRAME_INTERVAL_STATS_HPP_5B0E3C8E_7D5A_4B7E_9F0B_2C4E1B6A9D13

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <string>
#include <utility>

#include "debug.h"
#include "utils/color_out.h"

/**
 * @brief Tracks the time between consecutive frames passing a point in the pipeline and
 *        reports it periodically.
 */
class FrameIntervalStats {
public:
        using clock = std::chrono::steady_clock;

        static constexpr double LONG_INTERVAL_RATIO = 1.25;
        static constexpr double SHORT_INTERVAL_RATIO = 0.75;
        static constexpr std::chrono::seconds REPORT_INTERVAL{10};

        explicit FrameIntervalStats(std::string label) : label(std::move(label)) {}

        /// The next frame is not measured against the one preceding the call.
        void set_fps(double fps) {
                this->fps = fps;
                this->frame_time = fps > 0
                        ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / fps))
                        : std::chrono::nanoseconds::zero();
                this->has_prev_frame = false;
                this->reset_period();
        }

        void record(clock::time_point now = clock::now()) {
                this->frames++;
                if (!this->has_prev_frame) {
                        this->has_prev_frame = true;
                        this->prev_frame = now;
                        this->last_summary = now;
                        return;
                }

                auto interval = std::chrono::duration_cast<std::chrono::microseconds>(now - this->prev_frame);
                this->prev_frame = now;
                this->interval_max = std::max(this->interval_max, interval);
                this->interval_min = std::min(this->interval_min, interval);
                if (this->frame_time > std::chrono::nanoseconds::zero()) {
                        if (interval > LONG_INTERVAL_RATIO * this->frame_time) {
                                this->long_intervals++;
                        } else if (interval < SHORT_INTERVAL_RATIO * this->frame_time) {
                                this->short_intervals++;
                        }
                }

                if (now - this->last_summary > REPORT_INTERVAL) {
                        this->report();
                        this->reset_period();
                        this->last_summary = now;
                }
        }

        /// As record() but follows the frame rate of the stream.
        void record(double fps, clock::time_point now = clock::now()) {
                if (fps != this->fps) {
                        this->set_fps(fps);
                }
                this->record(now);
        }

        uint64_t get_frames() const {
                return this->frames;
        }

        uint64_t get_long_intervals() const {
                return this->long_intervals;
        }

        uint64_t get_short_intervals() const {
                return this->short_intervals;
        }

        std::chrono::microseconds get_interval_max() const {
                return this->interval_max;
        }

        std::chrono::microseconds get_interval_min() const {
                return this->interval_min;
        }

private:
        void report() const {
                LOG(LOG_LEVEL_INFO) << std::fixed << std::setprecision(1)
                                << SUNDERLINE(this->label << " (cumulative)")
                                << " - Total Video Frames: "
                                << SBOLD(this->frames)
                                << " / Long Intervals: "
                                << SBOLD(this->long_intervals)
                                << " / Short Intervals: "
                                << SBOLD(this->short_intervals)
                                << " / Max time diff video (ms): "
                                << SBOLD(this->interval_max.count() / 1000.0)
                                << " / Min time diff video (ms): "
                                << SBOLD(this->interval_min.count() / 1000.0)
                                << "\n";
        }

        void reset_period() {
                this->interval_max = std::chrono::microseconds::zero();
                this->interval_min = std::chrono::microseconds::max();
        }

        std::string label;
        double fps = 0;
        std::chrono::nanoseconds frame_time = std::chrono::nanoseconds::zero();
        uint64_t frames = 0;
        uint64_t long_intervals = 0;
        uint64_t short_intervals = 0;

        bool has_prev_frame = false;

        clock::time_point prev_frame{};
        clock::time_point last_summary{};

        // max/min cover only the time since the last report
        std::chrono::microseconds interval_max = std::chrono::microseconds::zero();
        std::chrono::microseconds interval_min = std::chrono::microseconds::max();
};

#endif // defined SRC_UTILS_FRAME_INTERVAL_STATS_HPP_5B0E3C8E_7D5A_4B7E_9F0B_2C4E1B6A9D13
