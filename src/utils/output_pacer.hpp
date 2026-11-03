/**
 * @file   utils/output_pacer.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_UTILS_OUTPUT_PACER_HPP_3F9C1A7E_6B2D_4E85_A1C4_8D0B5E7F2C96
#define SRC_UTILS_OUTPUT_PACER_HPP_3F9C1A7E_6B2D_4E85_A1C4_8D0B5E7F2C96

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>

#include "debug.h"
#include "utils/color_out.h"

/**
 * @brief Holds back decoded frames until a set number are queued, then hands one to the display
 *        every frame time.
 *
 * Used from the display thread only.
 */
class OutputPacer {
public:
        using clock = std::chrono::steady_clock;

        static constexpr std::chrono::seconds REPORT_INTERVAL{10};
        /// Interval change per frame the queue is away from its target, so the cadence follows the stream's clock.
        static constexpr double NUDGE_PER_FRAME = 100e-6;
        /// Half a frame below the depth, so the next frame usually arrives mid-way between hand-offs.
        static constexpr double TARGET_BELOW_DEPTH = 0.5;

        explicit OutputPacer(unsigned depth) : depth(depth) {}

        /// Starts priming again at the given frame rate.
        void reset(double fps) {
                this->frame_time = fps > 0
                        ? std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps))
                        : clock::duration::zero();
                this->start_priming();
        }

        /// How long the queue holds a frame, for delaying audio to match.
        std::chrono::milliseconds delay() const {
                return std::chrono::duration_cast<std::chrono::milliseconds>(this->depth * this->frame_time);
        }

        /// True until `queued` has reached the depth since the last reset or underrun.
        bool is_priming(size_t queued) {
                if (this->priming && queued >= this->depth) {
                        this->priming = false;
                }
                return this->priming;
        }

        /// Frames to discard before the next pop, leaving the depth queued.
        size_t excess(size_t queued) {
                if (this->priming || queued <= 2 * static_cast<size_t>(this->depth)) {
                        return 0;
                }
                size_t n = queued - this->depth;
                this->dropped += n;
                LOG(LOG_LEVEL_INFO) << "Display buffer stats - Dropped frames: " << SBOLD(n)
                                << " / Queued: " << SBOLD(queued) << "\n";
                return n;
        }

        /**
         * When to hand over the frame just popped.
         * @param queued frames left in the queue after the pop
         */
        clock::time_point due(size_t queued, clock::time_point now) {
                this->frames++;
                this->record_depth(queued);
                this->report_if_due(now);

                // If we haven't pulled the first frame yet, we won't know the next time it's due. Set the timing
                // to be the TARGET BELOW DEPTH percentage away of the frame.
                if (!this->next_due) {
                        // starts the queue at its target depth rather than waiting minutes for the nudge to get there
                        this->next_due = now + std::chrono::duration_cast<clock::duration>(this->frame_time * TARGET_BELOW_DEPTH);
                }
                // Check if we have passed the timing of the next frame by more than 50% of the duration of the frame.
                // If so, then reset priming as we are behind.
                else if (now - *this->next_due > this->frame_time / 2) {
                        this->underruns++;
                        this->start_priming();
                        double late_ms = std::chrono::duration<double, std::milli>(now - *this->next_due).count();
                        LOG(LOG_LEVEL_INFO) << std::fixed << std::setprecision(1)
                                        << "Display buffer stats - Underrun, late (ms): "
                                        << SBOLD(late_ms)
                                        << "\n";
                        return now;
                }

                // Take a copy of our current frame timing, then calculate the next frame timing. Offset the
                // timing against how quickly we're consuming the queue.
                auto handoff = *this->next_due;
                auto behind = this->depth - TARGET_BELOW_DEPTH - static_cast<double>(queued);
                *this->next_due += this->frame_time
                        + std::chrono::duration_cast<clock::duration>(this->frame_time * (NUDGE_PER_FRAME * behind));
                return handoff;
        }

        uint64_t get_frames() const {
                return this->frames;
        }

        uint64_t get_underruns() const {
                return this->underruns;
        }

        uint64_t get_dropped() const {
                return this->dropped;
        }

private:
        void start_priming() {
                this->priming = true;
                this->next_due.reset();
        }

        void record_depth(size_t queued) {
                this->period_frames++;
                this->period_depth_total += queued;
                this->depth_max = std::max(this->depth_max, queued);
                this->depth_min = std::min(this->depth_min, queued);
        }

        void report_if_due(clock::time_point now) {
                if (!this->has_report_start) {
                        this->has_report_start = true;
                        this->last_summary = now;
                        return;
                }
                if (now - this->last_summary <= REPORT_INTERVAL) {
                        return;
                }
                LOG(LOG_LEVEL_INFO) << std::fixed << std::setprecision(1)
                                << SUNDERLINE("Display buffer stats (cumulative)")
                                << " - Total Video Frames: " << SBOLD(this->frames)
                                << " / Underruns: " << SBOLD(this->underruns)
                                << " / Buffer Full Drops: " << SBOLD(this->dropped)
                                << " / Max depth: " << SBOLD(this->depth_max)
                                << " / Mean depth: " << SBOLD(static_cast<double>(this->period_depth_total) / this->period_frames)
                                << " / Min depth: " << SBOLD(this->depth_min)
                                << "\n";
                this->period_frames = 0;
                this->period_depth_total = 0;
                this->depth_max = 0;
                this->depth_min = std::numeric_limits<size_t>::max();
                this->last_summary = now;
        }

        unsigned depth;
        clock::duration frame_time = clock::duration::zero();
        bool priming = true;
        std::optional<clock::time_point> next_due;

        uint64_t frames = 0;
        uint64_t underruns = 0;
        uint64_t dropped = 0;

        bool has_report_start = false;
        clock::time_point last_summary{};

        // depth max/mean/min cover only the time since the last report
        uint64_t period_frames = 0;
        uint64_t period_depth_total = 0;
        size_t depth_max = 0;
        size_t depth_min = std::numeric_limits<size_t>::max();
};

#endif // defined SRC_UTILS_OUTPUT_PACER_HPP_3F9C1A7E_6B2D_4E85_A1C4_8D0B5E7F2C96
