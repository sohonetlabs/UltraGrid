/**
 * @file   utils/stage_latency_stats.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_UTILS_STAGE_LATENCY_STATS_HPP_8E2A4C61_3B7D_4F19_A6C0_5D1E9B7F2A48
#define SRC_UTILS_STAGE_LATENCY_STATS_HPP_8E2A4C61_3B7D_4F19_A6C0_5D1E9B7F2A48

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <string>
#include <utility>

#include "debug.h"
#include "utils/color_out.h"

/**
 * @brief Tracks how long frames spend in a stage of the pipeline and reports it periodically.
 *
 * Safe to record from several threads at once.
 */
class StageLatencyStats {
public:
        using clock = std::chrono::steady_clock;

        static constexpr std::chrono::seconds REPORT_INTERVAL{10};

        explicit StageLatencyStats(std::string label) : label(std::move(label)) {}

        /// A frame is over the frame time only when `fps` is known (greater than zero).
        void record(clock::duration latency, double fps, clock::time_point now = clock::now()) {
                std::lock_guard<std::mutex> lk(this->lock);
                auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(latency);

                this->frames++;
                this->period_frames++;
                this->period_total += latency_us;
                this->latency_max = std::max(this->latency_max, latency_us);
                this->latency_min = std::min(this->latency_min, latency_us);
                if (fps > 0 && latency > std::chrono::duration<double>(1.0 / fps)) {
                        this->over_frame_time++;
                }

                if (!this->has_report_start) {
                        this->has_report_start = true;
                        this->last_summary = now;
                } else if (now - this->last_summary > REPORT_INTERVAL) {
                        this->report();
                        this->reset_period();
                        this->last_summary = now;
                }
        }

        uint64_t get_frames() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->frames;
        }

        uint64_t get_over_frame_time() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->over_frame_time;
        }

        std::chrono::microseconds get_latency_max() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->latency_max;
        }

        std::chrono::microseconds get_latency_min() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->latency_min;
        }

        std::chrono::microseconds get_latency_mean() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->period_mean();
        }

private:
        std::chrono::microseconds period_mean() const {
                return this->period_frames > 0
                        ? this->period_total / static_cast<int64_t>(this->period_frames)
                        : std::chrono::microseconds::zero();
        }

        void report() const {
                LOG(LOG_LEVEL_INFO) << std::fixed << std::setprecision(1)
                                << SUNDERLINE(this->label << " (cumulative)")
                                << " - Total Video Frames: "
                                << SBOLD(this->frames)
                                << " / Over Frame Time: "
                                << SBOLD(this->over_frame_time)
                                << " / Max latency (ms): "
                                << SBOLD(this->latency_max.count() / 1000.0)
                                << " / Mean latency (ms): "
                                << SBOLD(this->period_mean().count() / 1000.0)
                                << " / Min latency (ms): "
                                << SBOLD(this->latency_min.count() / 1000.0)
                                << "\n";
        }

        void reset_period() {
                this->period_frames = 0;
                this->period_total = std::chrono::microseconds::zero();
                this->latency_max = std::chrono::microseconds::zero();
                this->latency_min = std::chrono::microseconds::max();
        }

        std::mutex lock;
        std::string label;
        uint64_t frames = 0;
        uint64_t over_frame_time = 0;

        bool has_report_start = false;
        clock::time_point last_summary{};

        // max/mean/min cover only the time since the last report
        uint64_t period_frames = 0;
        std::chrono::microseconds period_total = std::chrono::microseconds::zero();
        std::chrono::microseconds latency_max = std::chrono::microseconds::zero();
        std::chrono::microseconds latency_min = std::chrono::microseconds::max();
};

#endif // defined SRC_UTILS_STAGE_LATENCY_STATS_HPP_8E2A4C61_3B7D_4F19_A6C0_5D1E9B7F2A48
