#include "participant_db.hpp"

/**
 *  @brief The constructor for a participant.
 */
template<typename State>
Participant<State>::Participant(unsigned int ssrc, volatile int *delayMs, const std::string& streamId) : ssrc(ssrc) {
    // Mark the time this object was created as now
    this->creationTime = std::chrono::high_resolution_clock::now();

    // Create a playout buffer for the participant, passing stream identifier for log messages
    this->playoutBuffer = std::make_unique<PlayoutBuffer>(delayMs, streamId);

    // Initialise all SDES attributes as empty strings
    this->sdesCname    = std::string();
    this->sdesName     = std::string();
    this->sdesEmail    = std::string();
    this->sdesPhone    = std::string();
    this->sdesLocation = std::string();
    this->sdesTool     = std::string();
    this->sdesNote     = std::string();

    // Set the decoder state to be a nullptr
    this->decoderState = nullptr;
    this->decoderDelete = nullptr;
    
    // Set the packet type to be a packet type not in range
    this->packetType = 255;

    // Convert the creation time to a timespec
    auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(this->creationTime);
    // Create a TFRC state for the participant
    this->tfrcState = tfrc_init(std::chrono::duration_cast<std::chrono::nanoseconds>(seconds.time_since_epoch()).count());
}

/**
 *  @brief The destructor for the participant. This will ensure that the decoder state is appropiately released.
 *         There is no guarantee that the decoder deletion function actually released the resource, but otherwise
 *         this will lead to memory leaks. Therefore implementors should ENSURE that the state is released within
 *         the destruction function (states made up of std lib C++ objects can simply implement the following):
 *         
 *         void deleteState(State* state) {
 *             delete state;
 *         }
 */
template<typename State>
Participant<State>::~Participant() {
    if(this->decoderState && this->decoderDelete) {
        // The state has a custom deletion because although this pointer is "owned" by the participant
        // the pointers within the state, are not smart pointers, and requires a custom function. (This is because
        // of the requirement to integrate with C-style code).
        this->decoderDelete(this->decoderState);
    }

    // The TFRC state needs releasing as well
    tfrc_done(this->tfrcState);
}

/**
 *  @brief A setter for the state and it's deletion function. Because the state is an "unmanaged" pointer
 *         that this object accepts it requires an additional deletion function to ensure that resources
 *         owned by the state (and the state itself) are properly released. 
 * 
 *         01-03-2023 - Not an ideal situation, but the changes to enforce a better memory management model
 *                      would be too far reaching.
 */
template<typename State>
void Participant<State>::setState(State* state, std::function<void(State*)> stateDeletion) {
    // Set both the decoder state and the deletion function
    this->decoderState = state;
    this->decoderDelete = stateDeletion;
}

/**
 *  @brief A passthrough setter for the playout delay on the playout buffer
 */
template<typename State>
void Participant<State>::setPlayoutDelay(long long playoutDelay) {
    this->playoutBuffer->setPlayoutDelay(playoutDelay);
}

/**
 * @brief A getter for the state in the participant. The caller of this function is making a promise
 *        that they will not release the pointer, or any of its resources.
 */
template<typename State>
State* Participant<State>::getState() {
    return this->decoderState;
}

/**
 * @brief A getter for the TFRC state. The caller of this function is making a promise not to release
 *        the pointer as it is being managed by this object.
 */
template<typename State>
struct tfrc* Participant<State>::getTfrc() {
    return this->tfrcState;
}

/**
 * @brief A getter for the playout buffer.
 */
template<typename State>
std::unique_ptr<PlayoutBuffer>& Participant<State>::getPlayoutBuffer() {
    return this->playoutBuffer;
}

/**
 * @brief A setter for all of the SDES properties
*/
template<typename State>
void Participant<State>::processSdes(rtcp_sdes_item* sdesItem) {
    switch(sdesItem->type) {
        case RTCP_SDES_CNAME:
            this->sdesCname = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_NAME:
            this->sdesName = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_EMAIL:
            this->sdesEmail = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_PHONE:
            this->sdesPhone = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_LOC:
            this->sdesLocation = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_TOOL:
            this->sdesTool = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_NOTE:
            this->sdesNote = std::string(sdesItem->data, sdesItem->length);
            break;
        case RTCP_SDES_END:
        case RTCP_SDES_PRIV:
        default:
            // Get the hex representation of the SSRC
            std::stringstream hexSsrc;
            hexSsrc << std::hex << this->ssrc;
            // Get the hex representation of the sdes item type
            std::stringstream hexSdesType;
            hexSdesType << std::hex << sdesItem->type;
            LOG(LOG_LEVEL_DEBUG) << "Ignored unknown SDES item (type=" << hexSdesType.str() << ") from " << hexSsrc.str() << "\n";
    }
}

/**
 *  @brief The constructor for the participant database.
 */
template<typename State>
ParticipantDB<State>::ParticipantDB(volatile int* delayMs, const std::string& streamId) : delayMs(delayMs), streamIdentifier(streamId) {
    // Intialise the map
    this->participants = std::map<unsigned int, std::unique_ptr<Participant<State>>>();
}

/**
 *  @brief The begin iterator for the participant database
 */
template<typename State>
typename ParticipantDB<State>::iterator ParticipantDB<State>::begin() {
    return this->participants.begin();
}

/**
 *  @brief The end iterator for the participant database
 */
template<typename State>
typename ParticipantDB<State>::iterator ParticipantDB<State>::end() {
    return this->participants.end();
}

/**
 *  @brief The constant begin iterator for the participant database
 */
template<typename State>
typename ParticipantDB<State>::const_iterator ParticipantDB<State>::cbegin() const {
    return this->participants.cbegin();
} 

/**
 *  @brief The constant end iterator for the participant database
 */
template<typename State>
typename ParticipantDB<State>::const_iterator ParticipantDB<State>::cend() const {
    return this->participants.cend();
}

/**
 * @brief A function for creating and inserting a new participant into the participant database.
 */
template<typename State>
void ParticipantDB<State>::addParticipant(unsigned int ssrc) {
    LOG(LOG_LEVEL_INFO) << "[" << this->streamIdentifier << "] Adding a new participant: " << std::hex << ssrc << "\n";
    // Create the participant
    std::unique_ptr<Participant<State>> participant = std::make_unique<Participant<State>>(ssrc, this->delayMs, this->streamIdentifier);
    // Insert the participant into the map
    this->participants.insert(std::make_pair(ssrc, std::move(participant)));
}

/**
 * @brief A getter function for checking if there is a participant registered against a particular SSRC.
 */
template<typename State>
bool ParticipantDB<State>::exists(unsigned int ssrc) {
    // Use the find function and compare it to the final element in the container
    return this->participants.find(ssrc) != this->participants.end();
}

/**
 * @brief A getter function for a participant. 
 */
template<typename State>
std::optional<std::reference_wrapper<Participant<State>>> ParticipantDB<State>::getParticipant(unsigned int ssrc) {
    auto it = this->participants.find(ssrc);
    if (it != this->participants.end()) {
        return std::make_optional(std::ref(*(it->second.get())));
    }
    return std::nullopt;
}

template<typename State>
void ParticipantDB<State>::setState(unsigned int ssrc, State* state, std::function<void(State*)> stateDeletion) {
    // Grab the participant and assign it using a factory
    if(auto participant = this->getParticipant(ssrc)) {
        participant->get().setState(state, stateDeletion);
    }
    // The SSRC does not exist yet
    else {
        LOG(LOG_LEVEL_WARNING) << "Attempting to set the state against an participant that does not exist\n";
    }
}

template<typename State>
void ParticipantDB<State>::removeParticipant(unsigned int ssrc) {
    // Check if the participant exists before attempting to extract them
    if(this->exists(ssrc)) {
        this->participants.extract(ssrc);
    }
}