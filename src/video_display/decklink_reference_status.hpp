/**
 * @file   video_display/decklink_reference_status.hpp
 * @author Andrew Walker    <andrew.walker@sohonet.com>
 */

#ifndef SRC_VIDEO_DISPLAY_DECKLINK_REFERENCE_STATUS_HPP_3C9F1E27_6A4B_4D82_B5E0_9F2D7C1A8E63
#define SRC_VIDEO_DISPLAY_DECKLINK_REFERENCE_STATUS_HPP_3C9F1E27_6A4B_4D82_B5E0_9F2D7C1A8E63

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "debug.h"
#include "utils/color_out.h"

struct DecklinkReference {
        bool locked = false;
        std::string mode; ///< empty when no reference is detected

        bool operator==(const DecklinkReference &other) const {
                return locked == other.locked && mode == other.mode;
        }
        bool operator!=(const DecklinkReference &other) const {
                return !(*this == other);
        }
};

/**
 * @brief Tracks the reference (genlock) input of each output device, logging it when it changes
 *        and reporting it periodically.
 */
class DecklinkReferenceStatus {
public:
        using clock = std::chrono::steady_clock;

        static constexpr std::chrono::seconds POLL_INTERVAL{1};
        static constexpr std::chrono::seconds REPORT_INTERVAL{10};

        /// The next update of each device is logged even if it is unchanged.
        void reset(int devices) {
                this->devices.resize(devices);
                for (auto &d : this->devices) {
                        d.log_next = true;
                }
                this->has_polled = false;
        }

        bool poll_due(clock::time_point now = clock::now()) const {
                return !this->has_polled || now - this->last_poll >= POLL_INTERVAL;
        }

        /// @param temperature_c card temperature, if the device reports it
        void update(int device, const DecklinkReference &reference, std::optional<int64_t> temperature_c,
                        clock::time_point now = clock::now()) {
                if (static_cast<size_t>(device) >= this->devices.size()) {
                        this->devices.resize(device + 1);
                }
                auto &d = this->devices[device];
                bool changed = d.reference && *d.reference != reference;
                if (changed) {
                        this->changes++;
                }
                if (changed || d.log_next) {
                        LOG(LOG_LEVEL_INFO) << "[Decklink display] Device " << device << " reference: "
                                        << describe(reference) << "\n";
                }
                d.reference = reference;
                d.log_next = false;
                d.temperature_c = temperature_c;

                if (!this->has_polled) {
                        this->has_polled = true;
                        this->last_summary = now;
                }
                this->last_poll = now;
        }

        void report_if_due(clock::time_point now = clock::now()) {
                if (!this->has_polled || now - this->last_summary <= REPORT_INTERVAL) {
                        return;
                }
                this->last_summary = now;

                std::ostringstream devices_out;
                for (size_t i = 0; i < this->devices.size(); ++i) {
                        const auto &d = this->devices[i];
                        devices_out << (i == 0 ? " - " : " / ") << "Device " << i << ": "
                                        << SBOLD((d.reference ? describe(*d.reference) : "unknown"))
                                        << ", Card temperature (C): "
                                        << SBOLD((d.temperature_c ? std::to_string(*d.temperature_c) : "unknown"));
                }
                LOG(LOG_LEVEL_INFO) << SUNDERLINE("Decklink reference status (cumulative)")
                                << devices_out.str()
                                << " / Reference changes: " << SBOLD(this->changes) << "\n";
        }

        uint64_t get_changes() const {
                return this->changes;
        }

        static std::string describe(const DecklinkReference &reference) {
                if (reference.mode.empty()) {
                        return reference.locked ? "locked" : "none";
                }
                return std::string(reference.locked ? "locked" : "not locked") + " (" + reference.mode + ")";
        }

private:
        struct Device {
                std::optional<DecklinkReference> reference;
                std::optional<int64_t> temperature_c;
                bool log_next = true;
        };

        std::vector<Device> devices;
        uint64_t changes = 0;

        bool has_polled = false;
        clock::time_point last_poll{};
        clock::time_point last_summary{};
};

#endif // defined SRC_VIDEO_DISPLAY_DECKLINK_REFERENCE_STATUS_HPP_3C9F1E27_6A4B_4D82_B5E0_9F2D7C1A8E63
