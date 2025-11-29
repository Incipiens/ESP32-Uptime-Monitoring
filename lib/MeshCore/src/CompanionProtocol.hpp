#pragma once

#include "FrameCodec.hpp"
#include "BLECentralTransport.hpp"
#include <Arduino.h>
#include <array>
#include <functional>

/**
 * CompanionProtocol - MeshCore Companion Radio Protocol implementation
 * 
 * This class implements the MeshCore protocol layer including:
 * - Command/response handling
 * - State machine: Disconnected → Connected → DeviceQueried → SessionReady
 * - High-level API for sending messages
 */
class CompanionProtocol {
public:
    // MeshCore Companion Radio Protocol Commands
    static constexpr uint8_t CMD_APP_START = 1;
    static constexpr uint8_t CMD_SEND_TXT_MSG = 2;
    static constexpr uint8_t CMD_SEND_CHANNEL_TXT_MSG = 3;
    static constexpr uint8_t CMD_SYNC_NEXT_MESSAGE = 6;
    static constexpr uint8_t CMD_GET_CHANNEL = 31;   // 0x1F
    static constexpr uint8_t CMD_DEVICE_QUERY = 22;  // 0x16 (spelled QEURY in MeshCore)

    // MeshCore Protocol Response Codes
    static constexpr uint8_t RESP_CODE_OK = 0;
    static constexpr uint8_t RESP_CODE_ERR = 1;
    static constexpr uint8_t RESP_CODE_SELF_INFO = 5;
    static constexpr uint8_t RESP_CODE_SENT = 6;
    static constexpr uint8_t RESP_CODE_DEVICE_INFO = 13;
    static constexpr uint8_t RESP_CODE_CHANNEL_INFO = 18;  // 0x12
    static constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV = 8;
    static constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV_V3 = 9;
    static constexpr uint8_t RESP_CODE_CHANNEL_MSG_RECV = 10;
    static constexpr uint8_t RESP_CODE_CHANNEL_MSG_RECV_V3 = 11;

    // Push Codes (unsolicited messages from device)
    static constexpr uint8_t PUSH_CODE_MSG_WAITING = 14;
    static constexpr uint8_t PUSH_CODE_SEND_CONFIRMED = 15;

    // Text types
    static constexpr uint8_t TXT_TYPE_PLAIN = 0;
    static constexpr uint8_t TXT_TYPE_SIGNED = 1;

    // Protocol limits
    static constexpr size_t APP_START_RESERVED_SIZE = 6;
    static constexpr size_t MAX_TEXT_MESSAGE_LEN = 140;
    static constexpr uint8_t MAX_MESH_CHANNELS = 8;
    static constexpr size_t MAX_RX_BUFFER_SIZE = 256;

    // Protocol state machine
    enum class State {
        Disconnected,
        Connected,
        DeviceQueried,   // After CMD_DEVICE_QUERY response
        SessionReady     // After CMD_APP_START response
    };

    // Callback for protocol state changes
    using StateCallback = std::function<void(State newState)>;
    
    // Callback for received messages
    using MessageCallback = std::function<void(uint8_t channelIndex, const String& message)>;

    /**
     * Construct CompanionProtocol with transport and codec
     * @param transport The BLE transport layer
     * @param codec The frame codec layer
     */
    CompanionProtocol(BLECentralTransport& transport, FrameCodec& codec);

    /**
     * Get current protocol state
     * @return Current state
     */
    State getState() const { return m_state; }

    /**
     * Get state as string for debugging
     * @return State name string
     */
    const char* getStateString() const;

    /**
     * Called when BLE connection is established
     */
    void onConnected();

    /**
     * Called when BLE connection is lost
     */
    void onDisconnected();

    /**
     * Start a session with the MeshCore device
     * Sends CMD_DEVICE_QUERY followed by CMD_APP_START
     * @param appName Application name to send
     * @return true if session started successfully
     */
    bool startSession(const String& appName);

    /**
     * Send a text message to a channel
     * @param channelIndex Channel index to send to
     * @param message Message text
     * @return true if message was sent successfully
     */
    bool sendTextMessageToChannel(uint8_t channelIndex, const String& message);

    /**
     * Find a channel by name and get its index
     * @param channelName Name of channel to find
     * @param outIndex Output: channel index if found
     * @return true if channel was found
     */
    bool findChannelByName(const String& channelName, uint8_t& outIndex);

    /**
     * Check if channel is ready for sending
     * @return true if channel is provisioned and ready
     */
    bool isChannelReady() const { return m_channelReady; }

    /**
     * Get configured channel index
     * @return Channel index
     */
    uint8_t getChannelIndex() const { return m_channelIndex; }

    /**
     * Set callback for state changes
     * @param callback Function to call on state change
     */
    void setStateCallback(StateCallback callback);

    /**
     * Set callback for received messages
     * @param callback Function to call when message is received
     */
    void setMessageCallback(MessageCallback callback);

    /**
     * Get last error message
     * @return Last error string
     */
    const String& getLastError() const { return m_lastError; }

    /**
     * Wait for a response from the device
     * @param timeoutMs Timeout in milliseconds
     * @return true if response was received
     */
    bool waitForResponse(unsigned long timeoutMs = 5000);

    /**
     * Get last response code received
     * @return Last response code
     */
    uint8_t getLastResponseCode() const { return m_lastResponseCode; }

private:
    void onFrame(uint8_t cmd, const uint8_t* payload, size_t payloadLen);
    void setState(State newState);

    BLECentralTransport& m_transport;
    FrameCodec& m_codec;
    
    State m_state = State::Disconnected;
    StateCallback m_stateCallback;
    MessageCallback m_messageCallback;
    
    // Response handling
    uint8_t m_rxBuffer[MAX_RX_BUFFER_SIZE];
    size_t m_rxPayloadLen = 0;
    volatile bool m_responseReceived = false;
    uint8_t m_lastResponseCode = 0xFF;
    
    // Channel info
    bool m_channelReady = false;
    uint8_t m_channelIndex = 0;
    uint8_t m_protocolVersion = 3;
    
    String m_lastError;
};
