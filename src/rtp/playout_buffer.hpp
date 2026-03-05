//
// Created by Sohonet on 2/16/23.
//

#ifndef ULTRAGRID_PLAYOUT_BUFFER_H
#define ULTRAGRID_PLAYOUT_BUFFER_H

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <list>
#include <mutex>
#include <limits>
#include <string>
#include <vector>

#include "rtp/rtp.h"

class PlayoutBufferStats {
public:
    PlayoutBufferStats();

    [[maybe_unused]] [[maybe_unused]] PlayoutBufferStats(const PlayoutBufferStats& playoutStats);
    PlayoutBufferStats& operator=(const PlayoutBufferStats &);

    void processStats(const std::unique_ptr<rtp_packet>& packet);

    long long getReceivedPacketsTotal();
    long long getExpectedPacketsTotal();
private:
    // Allocate enough bits in an array to represent all possible packets a sequence number could represent.
    // In a 64-bit system this produces an array of size 1024.
    unsigned long long packets[(std::numeric_limits<uint16_t>::max() + 1) / sizeof(unsigned long long) / 8];

    int statsInterval = 128;                // The number of bytes to be used as an interval for checking
                                            // packets

    int lastReportSequence;                 // The last sequence number seen during a reporting intervl
    unsigned short lastSequence;            // The last sequence number seen by the stats reporting
    int longestGap;

    std::atomic<int> receivedPackets;                    // The amount of actually received packets within the reporting interval
    std::atomic<int> expectedPackets;                    // The amount of expected packets within the reporting interval
    std::atomic<long long> receivedPacketsTotal;         // The total cumulative amount of received packets
    std::atomic<long long> expectedPacketsTotal;         // The total cumulative amount of expected packets
    
    std::chrono::high_resolution_clock::time_point lastDisplayTimestamp;      // The last seen display timestamp
    std::atomic<unsigned int> outOfOrderPackets;         // The number of out or order packets seen
    std::atomic<unsigned int> maxOutOfOrderPacketsDist;  // The maximum distribution of the out of order packets
    std::atomic<unsigned int> duplicatePackets;          // The number of duplicate packets found

    std::mutex statsMutex;                      // A mutex that is used to lock this object while it's updating the stats
};

class BufferFrame {
public:
    BufferFrame(unsigned int rtpTimestamp, long long playoutDelayUs);
    // Explicitly delete the copy constructor. Generally these objects should not be copied
    // because the RTP packet has no copy constructor itself, and therefore we would be 
    // duplicating pointers to data and there would be no clear schema for how to copy the RTP
    // packets.
    BufferFrame(const BufferFrame& frame) = delete;

    ~BufferFrame();

    // Set up the iterators so that outside classes can simply iterate over the top of the packet list
    using iterator = std::vector<std::unique_ptr<rtp_packet>>::iterator;
    using const_iterator = std::vector<std::unique_ptr<rtp_packet>>::const_iterator;

    // Create begin and end functions so that for iterations can happen on this class
    iterator begin();
    iterator end();

    [[maybe_unused]] [[nodiscard]] const_iterator cbegin() const;
    [[nodiscard]] const_iterator cend() const;

    void insertPacket(std::unique_ptr<rtp_packet> packet);
    void insertPacketOrdered(std::unique_ptr<rtp_packet> packet);

    // Getters
    size_t size();
    unsigned int getRtpTimestamp();
    bool isComplete();
    bool isReady();
    bool isOverdue();
    std::chrono::high_resolution_clock::time_point getDeletionTime();
    std::optional<std::reference_wrapper<rtp_packet>> getPacket(size_t index);
    // Setters
    void markComplete();
private:
    std::vector<std::unique_ptr<rtp_packet>> packets;              // A frame is made up of an ordered list of all
                                                                 // of the packets assigned to it (based on the RTP timestamp)

    unsigned int rtpTimestamp;                                   // The RTP timestamp of the frame
    std::chrono::high_resolution_clock::time_point arrivalTime;  // Arrival time of the first packet in the frame
    std::chrono::high_resolution_clock::time_point playoutTime;  // The expected playout time of the frame
    std::chrono::high_resolution_clock::time_point deletionTime; // The deletion time of the frame
    
    bool decoded      = false; // If the frame has been decoded or not.
    bool mBit         = false; // If the 'm' bit has been set on the frame (if the last packet has arrived or not).
    bool completed    = false; // If all of the packets for the frame have arrived.
    unsigned int magic;        // A magic number used for debugging purposes.

    std::mutex frameMutex;                      // A mutex that is used to lock this object while it's list of 
                                                // packets is being updated.
};

class PlayoutBuffer {
public:
    // ~10 seconds at 90kHz RTP clock — used to detect 32-bit timestamp wraparound
    static constexpr unsigned int WRAPAROUND_THRESHOLD = 900000;

    explicit PlayoutBuffer(volatile int* delayMs, const std::string& streamId = "unknown");

    void insert(rtp_packet* packet);
    std::unique_ptr<BufferFrame> popNextDisplayReadyFrame();
    void remove();

    void setPlayoutDelay(long long playoutDelayUs);

    PlayoutBufferStats getStats();
    bool isFrameReady();
    bool isEmpty();
private:
    void createFrame(std::unique_ptr<rtp_packet> packet, bool shouldLock);

    std::list<std::unique_ptr<BufferFrame>> frames; // A list of frames

    long long playoutDelayUs; // The delay for playing out in nanoseconds
    volatile int *offsetMs;   // The offset in milliseconds

    std::string streamIdentifier; // Identifies which stream this buffer belongs to

    std::mutex frameMutex;    // A mutex used for locking the frame list before
                              // it is accessed

    PlayoutBufferStats stats; // The stats object
};

#endif // ULTRAGRID_PLAYOUT_BUFFER_H
