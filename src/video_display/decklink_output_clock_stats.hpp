/**
 * @file   video_display/decklink_output_clock_stats.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_VIDEO_DISPLAY_DECKLINK_OUTPUT_CLOCK_STATS_HPP_6B2D9F14_3E7A_4C85_9A1F_0D4E8B2C7F31
#define SRC_VIDEO_DISPLAY_DECKLINK_OUTPUT_CLOCK_STATS_HPP_6B2D9F14_3E7A_4C85_9A1F_0D4E8B2C7F31

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>

#include "debug.h"
#include "utils/color_out.h"

/// A reading of the card's output clock, as returned by IDeckLinkOutput::GetHardwareReferenceClock.
struct DecklinkHardwareClock {
        int64_t time;
        int64_t time_in_frame;
        int64_t ticks_per_frame;
        int64_t timescale;
};

/**
 * @brief Tracks frames handed to the card against the card's own output clock and reports periodically
 *        how far the stream and card clocks differ.
 *
 * A slot is one output period of the card. Phase is how far into its slot a hand-off lands. Stream vs card is
 * the drift of the phase: negative when the stream runs slower than the card, so the card's buffer drains and
 * it repeats a frame each slip.
 *
 * Hand-offs aren't mapped to the slot they're shown in: in low-latency mode the card buffers frames, so the
 * hand-off slot doesn't say when a frame is shown.
 *
 * Safe to record from several threads at once.
 */
class DecklinkOutputClockStats {
public:
        using clock = std::chrono::steady_clock;
        using fractional_seconds = std::chrono::duration<double>;

        enum class Completion { completed, late, dropped, flushed };

        static constexpr std::chrono::seconds REPORT_INTERVAL{10};
        static constexpr std::chrono::milliseconds STALL{500};
        static constexpr double PHASE_SMOOTHING = 0.05;
        static constexpr std::chrono::seconds MIN_DRIFT_SPAN{30};
        static constexpr std::chrono::seconds DRIFT_RESEED_AFTER{1};

        /// Forgets the previous slot and clock references, e.g. when the output is reconfigured.
        void reset(bool low_latency) {
                std::lock_guard<std::mutex> lk(this->lock);
                this->low_latency = low_latency;
                this->has_prev = false;
                this->drift = {};
                this->has_card_ref = false;
        }

        void record_handoff(const DecklinkHardwareClock &hw, clock::duration display_call, clock::time_point now = clock::now()) {
                std::lock_guard<std::mutex> lk(this->lock);
                if (hw.ticks_per_frame <= 0 || hw.timescale <= 0) {
                        return;
                }
                this->frames++;
                this->period_frames++;

                double slot_us = 1e6 * hw.ticks_per_frame / hw.timescale;
                double phase_us = 1e6 * hw.time_in_frame / hw.timescale;
                int64_t slot = (hw.time - hw.time_in_frame) / hw.ticks_per_frame;

                if (this->has_prev) {
                        bool card_clock_reset = slot < this->prev_slot;
                        if (card_clock_reset || now - this->prev_now > STALL) {
                                // the stream resumes at an unrelated phase, which would bias the drift fit
                                this->drift.started = false;
                                this->has_card_ref = this->has_card_ref && !card_clock_reset;
                                this->stalls++;
                        }
                }
                this->has_prev = true;
                this->prev_slot = slot;
                this->prev_now = now;

                this->display_call_max = std::max(this->display_call_max,
                                std::chrono::duration_cast<std::chrono::microseconds>(display_call));

                this->track_drift(phase_us, slot_us, now);
                this->track_card_clock(hw, now);
                this->report_if_due(now);
        }

        void record_completion(Completion result) {
                std::lock_guard<std::mutex> lk(this->lock);
                switch (result) {
                case Completion::late: this->late++; break;
                case Completion::dropped: this->dropped++; break;
                case Completion::flushed: this->flushed++; break;
                case Completion::completed: break;
                }
        }

        void record_queue_depth(uint32_t frames) {
                std::lock_guard<std::mutex> lk(this->lock);
                this->queue_max = std::max(this->queue_max, frames);
                this->queue_min = std::min(this->queue_min, frames);
        }

        void record_handoff_drop(clock::time_point now = clock::now()) {
                std::lock_guard<std::mutex> lk(this->lock);
                this->handoff_drops++;
                this->report_if_due(now);
        }

        uint64_t get_frames() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->frames;
        }

        uint64_t get_stalls() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->stalls;
        }

        uint64_t get_late() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->late;
        }

        uint64_t get_dropped() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->dropped;
        }

        uint64_t get_flushed() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->flushed;
        }

        uint64_t get_handoff_drops() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->handoff_drops;
        }

        /// NaN until the drift has been measured for MIN_DRIFT_SPAN
        double get_stream_vs_card_ppm() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->stream_vs_card_ppm();
        }

        /// NaN until the card clock has been compared for MIN_DRIFT_SPAN
        double get_card_vs_host_ppm() {
                std::lock_guard<std::mutex> lk(this->lock);
                return this->card_vs_host_ppm;
        }

private:
        /**
         * Least-squares fit of the total offset against host time. The total offset is the phase plus one
         * slot for each slip since the fit started, so it rises steadily with drift instead of wrapping.
         */
        struct Drift {
                bool started = false;
                clock::time_point t0{};
                clock::time_point last_accepted{};
                double expected_phase_us = 0; ///< smoothed, includes slips
                double slot_us = 0;
                uint64_t n = 0;
                clock::duration last_elapsed{};
                double sum_t = 0;
                double sum_offset = 0;
                double sum_tt = 0;
                double sum_t_offset = 0;
        };

        void restart_drift(double phase_us, double slot_us, clock::time_point now) {
                auto &d = this->drift;
                d = {};
                d.started = true;
                d.t0 = now;
                d.last_accepted = now;
                d.expected_phase_us = phase_us;
                d.slot_us = slot_us;
        }

        void track_drift(double phase_us, double slot_us, clock::time_point now) {
                auto &d = this->drift;
                if (!d.started || d.slot_us != slot_us) {
                        this->restart_drift(phase_us, slot_us, now);
                }

                // Calculate the expected phase within a single slot duration
                double expected_in_slot_us = std::fmod(d.expected_phase_us, slot_us);
                if (expected_in_slot_us < 0) {
                        expected_in_slot_us += slot_us;
                }
                // Compare the expected phase with the actual phase and express this as the deviation
                // within a looping slot duration (-21,000 <= deviation <= +21,000 ~ for 24 FPS)
                double deviation_us = std::remainder(phase_us - expected_in_slot_us, slot_us);

                // Check if the deviation is within a quarter of the slot.
                // A single late or early hand-off is jitter, not drift
                if (std::fabs(deviation_us) > slot_us / 4) {
                        // A lasting jump would otherwise reject every later hand-off and freeze the fit
                        if (now - d.last_accepted <= DRIFT_RESEED_AFTER) {
                                return;
                        }
                        this->restart_drift(phase_us, slot_us, now);
                        deviation_us = 0;
                }
                // Our offset is the sum of our expectation and deviation. To avoid
                // drastically adjusting our expectations between samples, only smooth our expectation
                // by a percentage of the deviation.
                double offset_us = d.expected_phase_us + deviation_us;
                d.expected_phase_us += PHASE_SMOOTHING * deviation_us;
                d.last_accepted = now;

                // Keep a running sum of the results for a least-squares calculation of the clock drift.
                d.last_elapsed = now - d.t0;
                double t = fractional_seconds(d.last_elapsed).count();
                d.n++;
                d.sum_t += t;
                d.sum_offset += offset_us;
                d.sum_tt += t * t;
                d.sum_t_offset += t * offset_us;
        }

        double stream_vs_card_ppm() const {
                const auto &d = this->drift;
                // We cannot calculate drift with fewer than 2 samples.
                if (d.n < 2) {
                        return std::numeric_limits<double>::quiet_NaN();
                }
                double var_t = d.sum_tt - d.sum_t * d.sum_t / d.n;
                // If our divisor is zero or negative (due to rounding) OR not enough time has
                // passed since we started our fit, do not report.
                if (var_t <= 0 || d.last_elapsed < MIN_DRIFT_SPAN) {
                        return std::numeric_limits<double>::quiet_NaN();
                }
                // Offset drift in card microseconds per host second is the clock difference in ppm
                double slope = (d.sum_t_offset - d.sum_t * d.sum_offset / d.n) / var_t;
                return -slope;
        }

        void track_card_clock(const DecklinkHardwareClock &hw, clock::time_point now) {
                // Complete initialisation by storing our hardware clock and decklinks
                // hardware clock info.
                if (!this->has_card_ref) {
                        this->has_card_ref = true;
                        this->card_ref = hw.time;
                        this->card_ref_timescale = hw.timescale;
                        this->host_ref = now;
                        this->card_vs_host_ppm = std::numeric_limits<double>::quiet_NaN();
                        return;
                }

                // If the timescales we have don't match then the framerate has changed and
                // we need to reinitialise.
                if (hw.timescale != this->card_ref_timescale) {
                        this->has_card_ref = false;
                        return;
                }

                // If we haven't measured for the minimum drift span then return early.
                auto host_elapsed = now - this->host_ref;
                if (host_elapsed < MIN_DRIFT_SPAN) {
                        return;
                }

                // Calculate the ppm based on the timing between the host and card clocks
                double host_s = fractional_seconds(host_elapsed).count();
                double card_s = static_cast<double>(hw.time - this->card_ref) / hw.timescale;
                this->card_vs_host_ppm = ((card_s / host_s) - 1) * 1e6;
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
                this->report();
                this->reset_period();
                this->last_summary = now;
        }

        static std::string ppm(double v) {
                if (std::isnan(v)) {
                        return "-";
                }
                std::ostringstream out;
                out << std::fixed << std::setprecision(1) << v;
                return out.str();
        }

        std::string slip_interval() const {
                double drift_ppm = this->stream_vs_card_ppm();
                if (std::isnan(drift_ppm) || std::fabs(drift_ppm) < 0.1) {
                        return "-";
                }
                std::ostringstream out;
                out << std::fixed << std::setprecision(0) << this->drift.slot_us / std::fabs(drift_ppm);
                return out.str();
        }

        void report() const {
                std::ostringstream out;
                out << std::fixed << std::setprecision(1)
                                << SUNDERLINE("Decklink output clock stats (cumulative)")
                                << " - Total Video Frames: " << SBOLD(this->frames)
                                << " / Stalls: " << SBOLD(this->stalls);
                if (this->period_frames > 0) {
                        out << " / Max display call (ms): " << SBOLD(this->display_call_max.count() / 1000.0);
                }
                out << " / Stream vs card (ppm): " << SBOLD(ppm(this->stream_vs_card_ppm()))
                                << " / Slip every (s): " << SBOLD(this->slip_interval())
                                << " / Card vs host (ppm): " << SBOLD(ppm(this->card_vs_host_ppm));
                if (this->queue_min <= this->queue_max) {
                        out << " / Max queued: " << SBOLD(this->queue_max)
                                        << " / Min queued: " << SBOLD(this->queue_min);
                }
                if (!this->low_latency) {
                        out << " / Late: " << SBOLD(this->late)
                                        << " / Dropped: " << SBOLD(this->dropped)
                                        << " / Flushed: " << SBOLD(this->flushed);
                }
                out << " / Hand-off drops: " << SBOLD(this->handoff_drops);
                LOG(LOG_LEVEL_INFO) << out.str() << "\n";
        }

        void reset_period() {
                this->period_frames = 0;
                this->display_call_max = std::chrono::microseconds::zero();
                this->queue_max = 0;
                this->queue_min = std::numeric_limits<uint32_t>::max();
        }

        std::mutex lock;
        bool low_latency = true;

        uint64_t frames = 0;
        uint64_t stalls = 0;
        uint64_t late = 0;
        uint64_t dropped = 0;
        uint64_t flushed = 0;
        uint64_t handoff_drops = 0;

        bool has_prev = false;
        int64_t prev_slot = 0;
        clock::time_point prev_now{};

        Drift drift{};

        bool has_card_ref = false;
        int64_t card_ref = 0;
        int64_t card_ref_timescale = 0;
        clock::time_point host_ref{};
        double card_vs_host_ppm = std::numeric_limits<double>::quiet_NaN();

        bool has_report_start = false;
        clock::time_point last_summary{};

        // the rest cover only the time since the last report
        uint64_t period_frames = 0;
        std::chrono::microseconds display_call_max = std::chrono::microseconds::zero();
        uint32_t queue_max = 0;
        uint32_t queue_min = std::numeric_limits<uint32_t>::max();
};

#endif // defined SRC_VIDEO_DISPLAY_DECKLINK_OUTPUT_CLOCK_STATS_HPP_6B2D9F14_3E7A_4C85_9A1F_0D4E8B2C7F31
