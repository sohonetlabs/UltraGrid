//
// Created by Sohonet on 2/16/23.
//

#ifndef ULTRAGRID_PARTICIPANT_DB_H
#define ULTRAGRID_PARTICIPANT_DB_H

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"

#include "audio/types.h"
#include "debug.h"
#include "pdb.h"
#include "tfrc.h"
#include "tv.h"
#include "rtp/playout_buffer.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>

struct state_decoder;
struct state_audio_decoder;

// /**
//  * These structs are used to pass data between decoder and receiver.
//  */
// struct vcodec_state {
//         struct state_video_decoder *decoder;
//         unsigned int max_frame_size; // Maximal frame size to be returned to caller by a decoder to
//                                      // allow him adjust buffers accordingly
//         unsigned int decoded;
// };

// struct pbuf_audio_data {
//         audio_frame buffer;
//         struct sockaddr_storage source;      // Network source address
//         struct state_audio_decoder *decoder;

//         bool reconfigured;
//         size_t frame_size; // The currently decoded audio frame size (used similarly as vcodec_state::max_frame_size
//                            // to allow caller adjust buffers if needed)
// };

template<typename State>
class Participant {
public:
    Participant(unsigned int ssrc, volatile int *delayMs, const std::string& streamId = "unknown");
    ~Participant();

    State* getState();
    struct tfrc* getTfrc();
    std::unique_ptr<PlayoutBuffer>& getPlayoutBuffer();
    void processSdes(rtcp_sdes_item* sdesItem);

    void setState(State* state, std::function<void(State*)> stateDeletion);
    void setPlayoutDelay(long long playoutDelay);
private:
    unsigned int ssrc;
    // SDES (Session Description Protocol Security Descriptions)
    std::string sdesCname;
    std::string sdesName;
    std::string sdesEmail;
    std::string sdesPhone;
    std::string sdesLocation;
    std::string sdesTool;
    std::string sdesNote;

    // Store decoder state and deletion function
    State* decoderState;
    std::function<void(State*)> decoderDelete;

    unsigned char packetType;                                    // The last seen RTP payload type for this participant
    std::unique_ptr<PlayoutBuffer> playoutBuffer;                // Each participant has their own playout buffer
    struct tfrc* tfrcState;
    std::chrono::high_resolution_clock::time_point creationTime; // The time this participant was made
};

template<typename State>
class ParticipantDB {
public:
    explicit ParticipantDB(volatile int* delayMs, const std::string& streamId = "unknown");

    // Set up the iterators so that outside classes can simply iterate over the top of the participant DB
    using iterator = typename std::map<unsigned int, std::unique_ptr<Participant<State>>>::iterator;
    using const_iterator = typename std::map<unsigned int, std::unique_ptr<Participant<State>>>::const_iterator;

    // Create begin and end functions so that for iterations can happen on this class
    iterator begin();
    iterator end(); 
    [[nodiscard]] const_iterator cbegin() const;
    [[nodiscard]] const_iterator cend() const;

    // Operations around adding, getting, and removing participants.
    void addParticipant(unsigned int ssrc);
    std::optional<std::reference_wrapper<Participant<State>>> getParticipant(unsigned int ssrc);
    bool exists(unsigned int ssrc);
    void setState(unsigned int ssrc, State* state, std::function<void(State*)> stateDeletion);
    void removeParticipant(unsigned int ssrc);
private:
    std::map<unsigned int, std::unique_ptr<Participant<State>>> participants;
    volatile int* delayMs;
    std::string streamIdentifier;
};

// Explicitly declare our use of the participant DB with the states so it'll compile
template class Participant<vcodec_state>;
template class ParticipantDB<vcodec_state>;
template class Participant<pbuf_audio_data>;
template class ParticipantDB<pbuf_audio_data>;

#endif // ULTRAGRID_PARTICIPANT_DB_H