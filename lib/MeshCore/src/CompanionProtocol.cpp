#include "CompanionProtocol.hpp"

// Define static constexpr members for pre-C++17 ODR compliance
constexpr uint8_t CompanionProtocol::CMD_APP_START;
constexpr uint8_t CompanionProtocol::CMD_SEND_TXT_MSG;
constexpr uint8_t CompanionProtocol::CMD_SEND_CHANNEL_TXT_MSG;
constexpr uint8_t CompanionProtocol::CMD_SYNC_NEXT_MESSAGE;
constexpr uint8_t CompanionProtocol::CMD_GET_CHANNEL;
constexpr uint8_t CompanionProtocol::CMD_DEVICE_QUERY;
constexpr uint8_t CompanionProtocol::RESP_CODE_OK;
constexpr uint8_t CompanionProtocol::RESP_CODE_ERR;
constexpr uint8_t CompanionProtocol::RESP_CODE_SELF_INFO;
constexpr uint8_t CompanionProtocol::RESP_CODE_SENT;
constexpr uint8_t CompanionProtocol::RESP_CODE_DEVICE_INFO;
constexpr uint8_t CompanionProtocol::RESP_CODE_CHANNEL_INFO;
constexpr uint8_t CompanionProtocol::RESP_CODE_CONTACT_MSG_RECV;
constexpr uint8_t CompanionProtocol::RESP_CODE_CONTACT_MSG_RECV_V3;
constexpr uint8_t CompanionProtocol::RESP_CODE_CHANNEL_MSG_RECV;
constexpr uint8_t CompanionProtocol::RESP_CODE_CHANNEL_MSG_RECV_V3;
constexpr uint8_t CompanionProtocol::PUSH_CODE_MSG_WAITING;
constexpr uint8_t CompanionProtocol::PUSH_CODE_SEND_CONFIRMED;
constexpr uint8_t CompanionProtocol::TXT_TYPE_PLAIN;
constexpr uint8_t CompanionProtocol::TXT_TYPE_SIGNED;
constexpr size_t CompanionProtocol::APP_START_RESERVED_SIZE;
constexpr size_t CompanionProtocol::MAX_TEXT_MESSAGE_LEN;
constexpr uint8_t CompanionProtocol::MAX_MESH_CHANNELS;
constexpr size_t CompanionProtocol::MAX_RX_BUFFER_SIZE;

CompanionProtocol::CompanionProtocol(BLECentralTransport& transport, FrameCodec& codec)
    : m_transport(transport)
    , m_codec(codec)
{
    // Set up frame callback to handle responses
    m_codec.setFrameCallback([this](uint8_t cmd, const uint8_t* payload, size_t payloadLen) {
        onFrame(cmd, payload, payloadLen);
    });

    // Set up transport state callback
    m_transport.setStateCallback([this](bool connected) {
        if (connected) {
            onConnected();
        } else {
            onDisconnected();
        }
    });
}

const char* CompanionProtocol::getStateString() const {
    switch (m_state) {
        case State::Disconnected: return "Disconnected";
        case State::Connected: return "Connected";
        case State::DeviceQueried: return "DeviceQueried";
        case State::SessionReady: return "SessionReady";
        default: return "Unknown";
    }
}

void CompanionProtocol::setState(State newState) {
    if (m_state != newState) {
        Serial.printf("CompanionProtocol: state %s -> %s\n", 
                      getStateString(), 
                      [newState]() -> const char* {
                          switch (newState) {
                              case State::Disconnected: return "Disconnected";
                              case State::Connected: return "Connected";
                              case State::DeviceQueried: return "DeviceQueried";
                              case State::SessionReady: return "SessionReady";
                              default: return "Unknown";
                          }
                      }());
        m_state = newState;
        if (m_stateCallback) {
            m_stateCallback(m_state);
        }
    }
}

void CompanionProtocol::onConnected() {
    m_responseReceived = false;
    m_lastResponseCode = 0xFF;
    m_rxPayloadLen = 0;
    setState(State::Connected);
}

void CompanionProtocol::onDisconnected() {
    m_channelReady = false;
    m_responseReceived = false;
    m_lastResponseCode = 0xFF;
    m_rxPayloadLen = 0;
    setState(State::Disconnected);
}

void CompanionProtocol::onFrame(uint8_t cmd, const uint8_t* payload, size_t payloadLen) {
    Serial.printf("CompanionProtocol: received frame cmd=0x%02X, len=%d\n", cmd, (int)payloadLen);
    
    m_lastResponseCode = cmd;
    
    // Store payload if it fits
    if (payloadLen <= MAX_RX_BUFFER_SIZE - 1) {
        m_rxBuffer[0] = cmd;
        if (payload != nullptr && payloadLen > 0) {
            memcpy(m_rxBuffer + 1, payload, payloadLen);
        }
        m_rxPayloadLen = payloadLen + 1;  // Include command byte
    } else {
        Serial.printf("CompanionProtocol: payload too large (%d > %d)\n", 
                      (int)payloadLen, (int)(MAX_RX_BUFFER_SIZE - 1));
        m_rxPayloadLen = 0;
    }
    
    m_responseReceived = true;
    
    // Handle push messages (unsolicited)
    if (cmd == PUSH_CODE_MSG_WAITING || cmd == PUSH_CODE_SEND_CONFIRMED) {
        Serial.printf("CompanionProtocol: received push code 0x%02X\n", cmd);
    }
}

void CompanionProtocol::setStateCallback(StateCallback callback) {
    m_stateCallback = std::move(callback);
}

void CompanionProtocol::setMessageCallback(MessageCallback callback) {
    m_messageCallback = std::move(callback);
}

bool CompanionProtocol::waitForResponse(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (!m_responseReceived && (millis() - start) < timeoutMs) {
        delay(10);
    }
    return m_responseReceived;
}

bool CompanionProtocol::startSession(const String& appName) {
    if (m_state < State::Connected) {
        m_lastError = "Not connected";
        return false;
    }

    // Step 1: Send CMD_DEVICE_QUERY to negotiate protocol version
    if (m_state < State::DeviceQueried) {
        Serial.println("CompanionProtocol: Sending CMD_DEVICE_QUERY...");
        
        m_responseReceived = false;
        uint8_t payload[1] = { m_protocolVersion };
        if (!m_codec.sendFrame(CMD_DEVICE_QUERY, payload, 1)) {
            m_lastError = "Failed to send CMD_DEVICE_QUERY";
            return false;
        }

        if (!waitForResponse(5000)) {
            m_lastError = "Timeout waiting for RESP_CODE_DEVICE_INFO";
            return false;
        }

        if (m_lastResponseCode != RESP_CODE_DEVICE_INFO) {
            m_lastError = "Unexpected response to CMD_DEVICE_QUERY";
            Serial.printf("Expected RESP_CODE_DEVICE_INFO (0x%02X), got 0x%02X\n", 
                          RESP_CODE_DEVICE_INFO, m_lastResponseCode);
            return false;
        }

        setState(State::DeviceQueried);
        Serial.println("CompanionProtocol: Protocol negotiated successfully");
    }

    // Step 2: Send CMD_APP_START to start the application session
    if (m_state < State::SessionReady) {
        Serial.println("CompanionProtocol: Sending CMD_APP_START...");

        // CMD_APP_START payload: version (1), reserved (6), app_name (variable)
        size_t appNameLen = appName.length();
        size_t payloadLen = 1 + APP_START_RESERVED_SIZE + appNameLen;
        std::vector<uint8_t> payload;
        payload.reserve(payloadLen);
        
        payload.push_back(m_protocolVersion);  // version
        // Use insert for reserved bytes (more efficient than push_back loop)
        payload.insert(payload.end(), APP_START_RESERVED_SIZE, 0);
        // Use insert for app name (more efficient than push_back loop)
        payload.insert(payload.end(), appName.begin(), appName.end());

        m_responseReceived = false;
        if (!m_codec.sendFrame(CMD_APP_START, payload)) {
            m_lastError = "Failed to send CMD_APP_START";
            return false;
        }

        if (!waitForResponse(5000)) {
            m_lastError = "Timeout waiting for RESP_CODE_SELF_INFO";
            return false;
        }

        if (m_lastResponseCode != RESP_CODE_SELF_INFO) {
            m_lastError = "Unexpected response to CMD_APP_START";
            Serial.printf("Expected RESP_CODE_SELF_INFO (0x%02X), got 0x%02X\n", 
                          RESP_CODE_SELF_INFO, m_lastResponseCode);
            return false;
        }

        setState(State::SessionReady);
        Serial.println("CompanionProtocol: Application session started successfully");
    }

    m_lastError = "";
    return true;
}

bool CompanionProtocol::findChannelByName(const String& channelName, uint8_t& outIndex) {
    if (m_state < State::SessionReady) {
        m_lastError = "Session not ready";
        return false;
    }

    Serial.printf("CompanionProtocol: Searching for channel '%s'...\n", channelName.c_str());

    for (uint8_t queryIndex = 0; queryIndex < MAX_MESH_CHANNELS; queryIndex++) {
        uint8_t payload[1] = { queryIndex };
        
        m_responseReceived = false;
        if (!m_codec.sendFrame(CMD_GET_CHANNEL, payload, 1)) {
            m_lastError = "Failed to send CMD_GET_CHANNEL";
            return false;
        }

        if (!waitForResponse(5000)) {
            Serial.printf("No response for channel index %d, stopping search\n", queryIndex);
            break;
        }

        if (m_lastResponseCode == RESP_CODE_ERR) {
            Serial.printf("Channel index %d not found (RESP_CODE_ERR), stopping search\n", queryIndex);
            break;
        }

        if (m_lastResponseCode != RESP_CODE_CHANNEL_INFO) {
            Serial.printf("Unexpected response code 0x%02X for channel index %d\n", 
                          m_lastResponseCode, queryIndex);
            break;
        }

        // Parse channel info response
        // RESP_CODE_CHANNEL_INFO payload: response_code(1) + channel_index(1) + name(32) + secret(16)
        if (m_rxPayloadLen >= 34) {  // resp_code + index + name(32)
            uint8_t respChannelIndex = m_rxBuffer[1];
            
            // Extract channel name (32 bytes at offset 2)
            char foundName[33] = {0};
            memcpy(foundName, m_rxBuffer + 2, 32);
            foundName[32] = '\0';
            
            // Trim trailing spaces
            size_t nameLen = strlen(foundName);
            while (nameLen > 0 && foundName[nameLen-1] == ' ') {
                foundName[--nameLen] = '\0';
            }

            Serial.printf("Found channel %d: '%s'\n", respChannelIndex, foundName);

            if (String(foundName) == channelName) {
                outIndex = respChannelIndex;
                m_channelIndex = respChannelIndex;
                m_channelReady = true;
                Serial.printf("Matched! Using channel index %d for '%s'\n", 
                              m_channelIndex, channelName.c_str());
                m_lastError = "";
                return true;
            }
        }
    }

    m_lastError = "Channel '" + channelName + "' not found on device";
    Serial.println(m_lastError.c_str());
    return false;
}

bool CompanionProtocol::sendTextMessageToChannel(uint8_t channelIndex, const String& message) {
    if (m_state < State::SessionReady) {
        m_lastError = "Session not ready";
        return false;
    }

    size_t textLen = message.length();
    if (textLen > MAX_TEXT_MESSAGE_LEN) {
        textLen = MAX_TEXT_MESSAGE_LEN;
    }

    // CMD_SEND_CHANNEL_TXT_MSG: txt_type(1) + channel_index(1) + timestamp(4) + text
    const size_t TIMESTAMP_SIZE = 4;
    std::vector<uint8_t> payload;
    payload.reserve(1 + 1 + TIMESTAMP_SIZE + textLen);
    
    payload.push_back(TXT_TYPE_PLAIN);  // txt_type
    payload.push_back(channelIndex);     // channel_index
    
    // Timestamp - use 0 to indicate "now" (more efficient than loop)
    payload.insert(payload.end(), TIMESTAMP_SIZE, 0);
    
    // Message text (use insert for efficiency)
    payload.insert(payload.end(), message.begin(), message.begin() + textLen);

    m_responseReceived = false;
    if (!m_codec.sendFrame(CMD_SEND_CHANNEL_TXT_MSG, payload)) {
        m_lastError = "Failed to send message";
        return false;
    }

    // Wait for send confirmation
    if (!waitForResponse(5000)) {
        // No confirmation received, but frame was sent to BLE layer
        m_lastError = "No acknowledgment received (message may have been sent)";
        Serial.println("CompanionProtocol: no response for message (may still be sent)");
        return false;  // Return false to indicate unconfirmed
    }

    if (m_lastResponseCode == RESP_CODE_OK || m_lastResponseCode == RESP_CODE_SENT) {
        Serial.println("CompanionProtocol: message sent successfully");
        m_lastError = "";
        return true;
    }

    m_lastError = "Unexpected response code";
    Serial.printf("CompanionProtocol: unexpected response 0x%02X\n", m_lastResponseCode);
    return false;  // Return false for unexpected response
}
