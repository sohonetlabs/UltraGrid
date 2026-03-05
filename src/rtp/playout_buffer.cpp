#include "debug.h"
#include "playout_buffer.hpp"

#include <algorithm>
#include <functional>
#include <math.h>

#define DELAY_NANOSECONDS 0.032 * 1000 * 1000
#define PLAYOUT_BUFFER_MAGIC 0xcafebabe

/**
  *  @brief Constructor for the playout buffer to initialise the defaults
  */
PlayoutBuffer::PlayoutBuffer(volatile int* delayMs, const std::string& streamId) : offsetMs(delayMs), streamIdentifier(streamId) {
    // Playout delay... should really be adaptive, based on the
    // jitter, but we use a (conservative) fixed 32ms delay for
    // now (2 video frames at 60fps).
    this->playoutDelayUs = DELAY_NANOSECONDS;
    this->stats = PlayoutBufferStats();
}

void PlayoutBuffer::insert(rtp_packet* packet) {
    // Start by creating ownership for this data - RTP implementation makes clear that
    // this data is being handed to the playout buffer and it is expected to free it once
    // it has finished using it.
    std::unique_ptr<rtp_packet> uPacket = std::unique_ptr<rtp_packet>(packet);

    // Capture any statistics about the packet
    this->stats.processStats(uPacket);

    // Capture the packet timestamp so we can identify the correct frame to insert it into.
    unsigned int packetTimestamp = uPacket->ts;
    auto frameComparator = [packetTimestamp] (std::unique_ptr<BufferFrame>& frame) {
      return packetTimestamp == frame->getRtpTimestamp();
    };

    {
        // Try getting the lock used to access the frame list
        std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->frameMutex);
        // Find the matching frame for the packet
        auto frameIterator = std::find_if(this->frames.begin(), this->frames.end(), frameComparator);
        
        // Check to see if we found an existing frame. If so, perform an insertion
        if(frameIterator != this->frames.end()) {
            (*frameIterator)->insertPacket(std::move(uPacket));
        }
        // Check if the frames are empty
        else if(this->frames.empty()) {
            this->createFrame(std::move(uPacket), false);
        }
        // Check to see if the packet belongs to a new frame (with wraparound handling)
        else if(this->frames.back()->getRtpTimestamp() < packetTimestamp ||
                this->frames.back()->getRtpTimestamp() - packetTimestamp > UINT32_MAX - WRAPAROUND_THRESHOLD) {
            // Mark the last frame as complete so it will be recognised as being ready to leave
            this->frames.back()->markComplete();
            // Create and insert new frame, with packet
            this->createFrame(std::move(uPacket), false);
        }
        // Check to see if the packet belongs to a new frame in the middle of the playout buffer
        else if(this->frames.front()->getRtpTimestamp() < packetTimestamp) {
            // In order for this to happen something has to have gone quite horribly wrong. We should not
            // be receiving packets for a frame AFTER we have received packets for a later frame.
            this->createFrame(std::move(uPacket), false);
        }
        // Check to see if the packet belongs to an old frame that has already been played out (with wraparound handling)
        else if(this->frames.front()->getRtpTimestamp() > packetTimestamp ||
                packetTimestamp - this->frames.front()->getRtpTimestamp() > UINT32_MAX - WRAPAROUND_THRESHOLD) {
            // There is not a lot we can do in this implementation because the frame will already be played
            // out. Simply log that it's happened
            LOG(LOG_LEVEL_WARNING) << "[" << this->streamIdentifier << "][pbuf] Found a frame with timestamp: " << packetTimestamp << " - The frame has likely already been played. Discarding.\n";
        }
    }

    // Check if the packet we created has been passed on
    if(uPacket != nullptr) {
        // If it has not been passed on enter a log with the RTP Timestamp explaining that the packet
        // has been dropped.
        LOG(LOG_LEVEL_WARNING) << "[" << this->streamIdentifier << "] Discarding frame with timestamp: " << packetTimestamp << "\n";
    }
}

/**
 *  @brief A setter for setting the playout delay on the buffer
 */
void PlayoutBuffer::setPlayoutDelay(long long playoutDelayUs) {
    this->playoutDelayUs = playoutDelayUs;
}

/**
 * @brief A function for popping the next display ready frame from the playout buffer
 *        and moving it to the caller.
 */
std::unique_ptr<BufferFrame> PlayoutBuffer::popNextDisplayReadyFrame() {
    // Set up the variables before we grab the lock
    std::unique_ptr<BufferFrame> frame;
    // Set up a context in which to create the lock so that when we exit the context
    // the lock is automatically released.
    {
        // Try getting the lock used to access the frame list
        std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->frameMutex);

        // Force-complete frames whose playout time was more than 1 second ago.
        // This prevents incomplete frames from blocking the buffer forever if a stream stalls.
        for (auto& f : this->frames) {
            if (!f->isComplete() && f->isOverdue()) {
                f->markComplete();
                LOG(LOG_LEVEL_DEBUG) << "[" << this->streamIdentifier << "][pbuf] Force-completing stale frame (RTP TS=" << f->getRtpTimestamp() << ")\n";
            }
            if (!f->isReady()) {
                break; // Frames are ordered by time; no point checking further
            }
        }

        // Create a function that tests is a frame is "display ready". I.e: Has its playout time passed and is the frame "complete".
        auto frameDisplayReadyComparator = [] (std::unique_ptr<BufferFrame>& bufferFrame) {
            return bufferFrame->isReady() && bufferFrame->isComplete();
        };

        // Find the first instance of a frame that is ready to display
        auto firstDisplayReadyFrameIt = std::find_if(this->frames.begin(), this->frames.end(), frameDisplayReadyComparator);
        // Check that we found a frame
        if(firstDisplayReadyFrameIt != this->frames.end()) {
            // Derefeference the iterator and move the frame before erasing it from the frame list
            frame = std::move(*firstDisplayReadyFrameIt);
            this->frames.erase(firstDisplayReadyFrameIt);
        }
        else {
            // Create a null pointer and hand this back to represent there are not more ready frames
            frame = std::unique_ptr<BufferFrame>(nullptr);
        }
    }
    // Return the popped frame
    return frame;
}

/**
 * @brief Remove stale frames from the front of the buffer whose deletion time has passed.
 *        Prevents undecoded frames from accumulating indefinitely.
 */
void PlayoutBuffer::remove() {
    std::unique_lock<std::mutex> lock(this->frameMutex);
    auto now = std::chrono::high_resolution_clock::now();
    auto it = this->frames.begin();
    while (it != this->frames.end()) {
        if (now > (*it)->getDeletionTime() && (*it)->isComplete()) {
            it = this->frames.erase(it);
        } else {
            // The buffer is ordered, so once we see a non-expired frame we can stop
            break;
        }
    }
}

/**
 * @brief A helper function for creating a frame from the data in an RTP packet, adding the packet, and pushing the
 *        frame into the frames list. Optionally lets you use a local lock for the object (useful if the lock is
 *        acquired elsewhere at the same time).
*/
void PlayoutBuffer::createFrame(std::unique_ptr<rtp_packet> packet, bool localLock) {
    // Make the frame and insert the packet
    unsigned int packetTimestamp = packet->ts;
    std::unique_ptr<BufferFrame> frame = std::make_unique<BufferFrame>(packetTimestamp, this->playoutDelayUs);
    frame->insertPacket(std::move(packet));

    // Create a comparison function for finding the packet with the lowest timestamp
    auto frameComparator = [&frame] (std::unique_ptr<BufferFrame>& bufferFrame) {
        return frame->getRtpTimestamp() < bufferFrame->getRtpTimestamp();
    };

    // Set up a context in which to create the lock so that when we exit the context
    // the lock is automatically released.
    {
        // Try getting the lock used to access and write to the frame list
        std::unique_lock<std::mutex> lock;
        // Only use the lock if the outer function has asked for it. Useful in contexts where
        // the outer function may already hold a lock.
        if(localLock) {
            lock = std::unique_lock<std::mutex>(this->frameMutex);
        }

        // Find the correct position to insert the frame into the list
        auto insertPos = std::find_if(this->frames.begin(), this->frames.end(), frameComparator);

        // Check if we found a match or not for a position in the list
        if(insertPos != this->frames.end()) {
            this->frames.insert(insertPos, std::move(frame));
        }
        // If we did not, then push to the as the timestamp will be smaller than any already
        // in the list.
        else {
            this->frames.push_back(std::move(frame));
        }
    }
}

/**
 * @brief A getter for the statistics
*/
PlayoutBufferStats PlayoutBuffer::getStats() {
    return this->stats;
}

/**
 * @brief A getter for if the playout buffer contains any frames that are ready to decode
*/
bool PlayoutBuffer::isFrameReady() {
    bool isReadyFrame = false;
    // Set up a context in which to create the lock so that when we exit the context
    // the lock is automatically released.
    {
        // Try getting the lock used to access the frame list
        std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->frameMutex);

        // Loop over every frame and check if the timing is correct and the entire frame has been received
        for(std::unique_ptr<BufferFrame>& frame : this->frames) {
            if(frame->isReady()) {
                if(frame->isComplete()) {
                    isReadyFrame = true;
                    break;
                }
                else {
                    LOG(LOG_LEVEL_DEBUG) << "[" << this->streamIdentifier << "][pbuf] Frame not playing because it has not been marked as complete. RTP Timestamp: " << frame->getRtpTimestamp() << "\n";
                }
            }
        }
    }
    return isReadyFrame;
}

/**
 * @brief A getter for whether the frame list is empty or not
*/
bool PlayoutBuffer::isEmpty() {
    bool isEmpty;
    // Set up a context in which to create the lock so that when we exit the context
    // the lock is automatically released.
    {
        // Try getting the lock used to access the frame list
        std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->frameMutex);

        isEmpty = this->frames.size() == 0;
    }
    return isEmpty;
}

/**
 *  @brief The constructor for the buffer frame - This will set the timings for the frame based on the playout delay
 */
BufferFrame::BufferFrame(unsigned int rtpTimestamp, long long playoutDelayUs) : rtpTimestamp(rtpTimestamp), magic(PLAYOUT_BUFFER_MAGIC) {
    // Initialise an empty list of packets
    this->packets = std::vector<std::unique_ptr<rtp_packet>>();
    this->packets.reserve(20000);

    // Set the arrival time as now
    this->arrivalTime = std::chrono::high_resolution_clock::now();
    // Set the playout time as the arrival time plus the playout delay
    this->playoutTime = this->arrivalTime + std::chrono::nanoseconds(playoutDelayUs);
    // Set the deletion time as the playout time plus the playout delay again
    this->deletionTime = this->playoutTime + std::chrono::nanoseconds(playoutDelayUs);
}

BufferFrame::~BufferFrame() {
    size_t packetsSize = this->packets.size();
    for(size_t i = 0; i < packetsSize; i++) {
        // The packets, despite being unique pointers, are allocated using
        // malloc, so they should be freed with "free" then mark the pointer
        // as released from the ownership so a double free does not occur.
        free(this->packets[i].release());
    }
}

/**
 *  @brief Insert a packet into the list stored by the buffer frame.
 */
void BufferFrame::insertPacket(std::unique_ptr<rtp_packet> packet) {
    // Set up a context in which to create the lock so that when we exit the context
    // the lock is automatically released.
    {
        // Try getting the lock used to access the packet list
        std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->frameMutex);

        // Check to see if the m-bit was set on the packet (should be the last packet for the frame)
        if(packet->m) {
            this->mBit = true;
        }

        this->packets.push_back(std::move(packet));
    }
}

/**
 *  @brief Insert a packet into the list stored by the buffer frame. This should be
 *         in descending order for the sequence numbers.
 */
void BufferFrame::insertPacketOrdered(std::unique_ptr<rtp_packet> packet) {
    // Take a copy of the RTP Timestamp from the packet to feed into the lambda function.
    unsigned int packetTimestamp = packet->ts;

    // Create a comparison function for finding the packet with the lowest timestamp
    auto packetComparator = [packetTimestamp] (std::unique_ptr<rtp_packet>& packet) {
        return packetTimestamp > packet->ts;
    };


    // Set up a context in which to create the lock so that when we exit the context
    // the lock is automatically released.
    {
        // Try getting the lock used to access the packet list
        std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->frameMutex);

        // Find the correct position to insert the packet into the list
        auto insertPos = std::find_if(this->packets.begin(), this->packets.end(), packetComparator);

        // Check to see if the m-bit was set on the packet (should be the last packet for the frame)
        if(packet->m) {
            this->mBit = true;
        }

        // Check if we found a match or not for a position in the list
        if(insertPos != this->packets.end()) {
            this->packets.insert(insertPos, std::move(packet));
        }
        // If we did not, then push to the as the timestamp will be smaller than any already
        // in the list.
        else {
            this->packets.push_back(std::move(packet));
        }
    }
}

/**
 * @brief Getter for the RTP Timestamp
 */
unsigned int BufferFrame::getRtpTimestamp() {
    return this->rtpTimestamp;
}

/**
 * @brief Getter for if the frame is "complete" or not. This relies on seeing either
 *        the last packet in the frame (marked with the 'm' bit) or a packet from the
 *        next frame.
 */
bool BufferFrame::isComplete() {
    return this->completed || this->mBit;
}

/**
 * @brief A getter that informs whether the buffer frame is ready. This is completed by
 *        checking if the the playout time has passed
*/
bool BufferFrame::isReady() {
    return std::chrono::high_resolution_clock::now() > this->playoutTime;
}

/**
 * @brief Returns true if the frame's playout time was more than 1 second ago.
 *        Used to force-complete stale incomplete frames that would otherwise block the buffer.
 */
bool BufferFrame::isOverdue() {
    return std::chrono::high_resolution_clock::now() > this->playoutTime + std::chrono::seconds(1);
}

/**
 * @brief Getter for the deletion time of the frame
 */
std::chrono::high_resolution_clock::time_point BufferFrame::getDeletionTime() {
    return this->deletionTime;
}

/**
 * @brief Get the amount of packets stored within the frame buffer
*/
size_t BufferFrame::size() {
    return this->packets.size();
}

/**
 * @brief Get a reference to the selected RTP packet. This function does not use the lock for acquiring
 *        the reference.
 */
std::optional<std::reference_wrapper<rtp_packet>> BufferFrame::getPacket(size_t index) {
    if (index < this->packets.size()) {
        return std::make_optional(std::ref(*(this->packets[index].get())));
    }
    return std::nullopt;
}

/**
 * @brief Mark the frame as being "complete". This typically happens when the first
 *        packet for the next frame arrives.
 */
void BufferFrame::markComplete() {
    this->completed = true;
}

/**
 * @brief A passthrough function for the packets in the buffer frame.
 *        This means an iterator for loop can be used with this object.
 */
BufferFrame::iterator BufferFrame::begin() {
    return this->packets.begin();
}

/**
 * @brief A passthrough function for the packets in the buffer frame.
 *        This means an iterator for loop can be used with this object.
 */
BufferFrame::iterator BufferFrame::end() {
    return this->packets.end();
}

/**
 * @brief A passthrough function for the packets in the buffer frame.
 *        This means an iterator for loop can be used with this object.
 */
BufferFrame::const_iterator BufferFrame::cbegin() const {
    return this->packets.cbegin();
}

/**
 * @brief A passthrough function for the packets in the buffer frame.
 *        This means an iterator for loop can be used with this object.
 */
BufferFrame::const_iterator BufferFrame::cend() const {
    return this->packets.cend();
}

#define NUMBER_WORD_BYTES sizeof(unsigned long long)
#define NUMBER_WORD_BITS (NUMBER_WORD_BYTES * __CHAR_BIT__)
#define STAT_INT_MIN_DIVISOR (sizeof(unsigned long long) * __CHAR_BIT__)

/**
 * @brief Compute the longest gap between packets that are missing.
 *        Tracks accumulated_loss across word boundaries to catch gaps
 *        that span multiple words (e.g., 70+ consecutive lost packets).
*/
static int GetConsecutiveZeros(unsigned long long packets)
{
        int longest_gap = 0;
        if (packets == 0) {
                return (int)(sizeof packets * __CHAR_BIT__);
        }
        if (packets == ULLONG_MAX) {
                return 0;
        }
        longest_gap = std::max(longest_gap, __builtin_clzll(packets));
        while (packets != 0) {
                longest_gap = std::max(longest_gap, __builtin_ctzll(packets));
                packets >>= 1;
        }
        return longest_gap;
}

static void ComputeLongestGap(int *longestGap, int *accumulatedLoss, unsigned long long packets)
{
        *longestGap = std::max(*longestGap, GetConsecutiveZeros(packets));
        // trailing_zeros counts packets missing at the start of the word group,
        // leading_zeros counts packets missing at the end
        int leading_zeros = packets == 0 ? (int)NUMBER_WORD_BITS : __builtin_clzll(packets);
        int trailing_zeros = packets == 0 ? (int)NUMBER_WORD_BITS : __builtin_ctzll(packets);
        *accumulatedLoss += trailing_zeros;
        *longestGap = std::max(*longestGap, *accumulatedLoss);
        if (leading_zeros < (int)NUMBER_WORD_BITS) { // at least one packet present, reset the counter
                *accumulatedLoss = leading_zeros;
        }
}

/**
 *  @brief The constructor for the playout buffer statistics. This sets the defaults.
 */
PlayoutBufferStats::PlayoutBufferStats() {
    // Set this to a negative number so we can recognise the first time the stats object
    // has been referenced.
    this->lastReportSequence = -1;
    this->lastSequence = 0;

    // Set most stats to zero as a "empty" state before any data has been inserted into the
    // buffer.
    this->receivedPackets = 0;
    this->expectedPackets = 0;
    this->receivedPacketsTotal = 0;
    this->expectedPacketsTotal = 0;

    this->lastDisplayTimestamp = std::chrono::high_resolution_clock::now();
    this->outOfOrderPackets = 0;
    this->maxOutOfOrderPacketsDist = 0;
    this->duplicatePackets = 0;
    this->longestGap = 0;

    // Initialise all values in the stats to be zero
    size_t packetsSize = (std::numeric_limits<uint16_t>::max() + 1) / sizeof(unsigned long long) / 8;
    for(size_t i = 0; i < packetsSize; i++) {
        this->packets[i] = 0;
    }
}

/**
 *  @brief A copy constructor for the playout buffer stats. This is required because
 *         atomic ints do not implement copy constructors themselves. 
 */
PlayoutBufferStats::PlayoutBufferStats(const PlayoutBufferStats& playoutStats) {
    this->statsInterval = playoutStats.statsInterval;
    this->lastReportSequence = playoutStats.lastReportSequence;
    this->lastSequence = playoutStats.lastSequence;

    this->receivedPackets = playoutStats.receivedPackets.load();
    this->expectedPackets = playoutStats.expectedPackets.load();
    this->receivedPacketsTotal = playoutStats.receivedPacketsTotal.load();
    this->expectedPacketsTotal = playoutStats.expectedPacketsTotal.load();

    this->lastDisplayTimestamp = playoutStats.lastDisplayTimestamp;
    this->outOfOrderPackets = playoutStats.outOfOrderPackets.load();
    this->maxOutOfOrderPacketsDist = playoutStats.maxOutOfOrderPacketsDist.load();
    this->duplicatePackets = playoutStats.duplicatePackets.load();
}

/**
 * @brief An implementation of the assignment operator which uses the copy constructor
 *        to copy across the data.
 */
PlayoutBufferStats& PlayoutBufferStats::operator=(__attribute__((unused)) const PlayoutBufferStats& other) {
    return *this;
}

/**
 * @brief Process the stats for a particular packet by marking the packet as recieved in a bit field
 *        representing the sequence number it arrived with.
*/
void PlayoutBufferStats::processStats(const std::unique_ptr<rtp_packet>& packet) {
    // Acquire the lock for the object. These stats need to be updated serially
    std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(this->statsMutex);

    // Initialise calculations that are often reused
    int uint16MaxOverflow = std::numeric_limits<uint16_t>::max() + 1;
    int uint16MaxHalf = uint16MaxOverflow / 2;

    // Initialise any stats that require it
    if(this->lastReportSequence == -1) {
        // Set the last seen sequence to be one before this initial packet
        this->lastSequence = packet->seq - 1;
        // Set the last report sequence to be the lower block
        this->lastReportSequence = static_cast<int>(floor((double)packet->seq / (double)this->statsInterval) * this->statsInterval);
        // Loop through the packets and store the fact they've been "received" for this set of packets.
        for(uint16_t i = this->lastReportSequence; i != packet->seq; ++i) {
            unsigned long long currentBit = 1ull << (i % NUMBER_WORD_BITS);
            this->packets[i / NUMBER_WORD_BITS] |= currentBit;
        }
    }

    // Discover which bit this packet represents in the block we store
    unsigned long long currentBit = 1ull << (packet->seq % NUMBER_WORD_BITS);
    size_t packetPosition = packet->seq / NUMBER_WORD_BITS;
    // Discover if the packet is out of order by checking if there is a packet later in the listing that has already been flipped.
    // i.e:
    // current bit = 00001000
    // packetPosition = 00010111
    //                     ^
    //            This is an out-of-order
    //            packet we have received
    // 
    // Do the bitwise AND on the negated current bit, and packet position
    //   11110111 &
    //   00010111
    // = 00010111
    // 
    // Then check if the result is larger than the current bit:
    // 23 > 8
    // In this case we can see that there has been an out of order packet as the 16-bit was flipped.
    // Below does this, but at a larger scale for long long
    bool isOutOfOrderPacket = (this->packets[packetPosition] & ~currentBit) > currentBit;
    if(isOutOfOrderPacket) {
        this->outOfOrderPackets++;

        // Calculate the distance between the packets - Because sequence numbers loop, we cannot simply take the difference
        // between the sequence numbers, and instead it must be calculates as part of a function of the sequence loop barrier.
        int outOfOrderDist = ((packet->seq + uint16MaxOverflow) - this->lastSequence) % uint16MaxOverflow;
        // If the result of the calculation gives us a large number, then we need to adjust that to match the actual distribution
        outOfOrderDist = outOfOrderDist < uint16MaxHalf ? outOfOrderDist : abs(outOfOrderDist - uint16MaxOverflow);
        this->maxOutOfOrderPacketsDist = std::max(this->maxOutOfOrderPacketsDist.load(), (unsigned int) outOfOrderDist);
    }
    // Update the last sequence number, as the following stats do not care what the last sequence number is
    this->lastSequence = packet->seq;

    // Check to see if the bit position in the packets has already been set. If so, we have a duplicate packet.
    if(this->packets[packetPosition] & currentBit) {
        this->duplicatePackets++;
    }
    // Update the packet information so that the current packet is marked as having arrived
    this->packets[packetPosition] |= currentBit;

    auto dist = static_cast<uint16_t>(packet->seq - this->lastReportSequence);
    if((dist >= this->statsInterval * 2) && (dist < uint16MaxHalf)) {
        // Sum up only up to stats interval to be able to catch out-of-order packets.
        auto reportSeqUntil = (uint16_t)((floor((double)packet->seq / this->statsInterval) - 1) * this->statsInterval);
        int accumulatedLoss = 0;
        for(uint16_t i = this->lastReportSequence; i != reportSeqUntil; i += NUMBER_WORD_BITS) {
            this->expectedPackets += NUMBER_WORD_BITS;
            this->receivedPackets += __builtin_popcountll(this->packets[i / NUMBER_WORD_BITS]);
            ComputeLongestGap(&this->longestGap, &accumulatedLoss, this->packets[i / NUMBER_WORD_BITS]);
            this->packets[i / NUMBER_WORD_BITS] = 0;
        }
        this->expectedPacketsTotal += this->expectedPackets;
        this->receivedPacketsTotal += this->receivedPackets;
        this->lastReportSequence = reportSeqUntil;
    }

    // Print out the report if enough time has passed. Reset the variables
    std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
    unsigned int secondsSinceReport = std::chrono::duration_cast<std::chrono::seconds>(now - this->lastDisplayTimestamp).count();
    if(secondsSinceReport > 5 && this->expectedPackets > 0) {
        // Calculate packet loss percentage
        double lossPercentage = ((double) this->receivedPackets / (double) this->expectedPackets) * 100;
        int lostPackets = this->expectedPackets - this->receivedPackets;
        
        std::stringstream output;
        // Form the loss percentage with a certain precision
        std::stringstream lossString;
        lossString << std::setprecision(4) << lossPercentage;
        // Form the SSRC as a hex string
        std::stringstream ssrcHex;
        ssrcHex << std::hex << packet->ssrc;
        // Put the initial message in
        output << "SSRC 0x" << ssrcHex.str() << ": ";
        output << this->receivedPackets <<  "/" << this->expectedPackets << " packets received ";
        output << "(" << lossString.str() << "%), " << lostPackets << " lost, max loss " << this->longestGap;

        // Put in a message about out of order packets
        if(this->outOfOrderPackets > 0) {
            output << ", reordered " << this->outOfOrderPackets << " (max dist " << this->maxOutOfOrderPacketsDist << ")";
        }

        // Put in a message about duplicate packets
        if(this->duplicatePackets > 0) {
            output << ", " << this->duplicatePackets << " dups";
        }

        // Log the stats
        LOG(LOG_LEVEL_INFO) << output.str() << "\n";

        // If we have a large enough out of order packet distribution, then we might accidently be 
        // marking packets as lost rather than out of order.
        if((int)this->maxOutOfOrderPacketsDist >= this->statsInterval) {
            size_t newStatsInterval = (size_t) floor((double)(this->maxOutOfOrderPacketsDist + STAT_INT_MIN_DIVISOR - 1) / (double)STAT_INT_MIN_DIVISOR) * STAT_INT_MIN_DIVISOR;
            if(newStatsInterval < (1U<<14U)) {
                this->statsInterval = static_cast<int>(newStatsInterval);
                LOG(LOG_LEVEL_VERBOSE) << "Adjusting stats interval to " << this->statsInterval << "\n";
            }
        }

        // Reset the variables that need resetting between reports
        this->expectedPackets = 0;
        this->receivedPackets = 0;
        this->longestGap = 0;
        this->outOfOrderPackets = 0;
        this->maxOutOfOrderPacketsDist = 0;
        this->duplicatePackets = 0;
        this->lastDisplayTimestamp = now;
    }
}

/**
 * @brief Getter for total received packets recorded by the stats
*/
long long PlayoutBufferStats::getReceivedPacketsTotal() {
    return this->receivedPacketsTotal.load();
}

/**
 * @brief Getter for total expected packets recorded by the stats
*/
long long PlayoutBufferStats::getExpectedPacketsTotal() {
    return this->expectedPacketsTotal.load();
}