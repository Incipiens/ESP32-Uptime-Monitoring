#include "FrameCodec.hpp"
#include <Arduino.h>

// Define static constexpr members for pre-C++17 ODR compliance
constexpr size_t FrameCodec::MAX_FRAME_PAYLOAD;

FrameCodec::FrameCodec(IByteTransport& transport)
    : m_transport(transport)
{
    // Set up the transport to forward received bytes to this codec
    m_transport.setRxCallback([this](const uint8_t* data, size_t len) {
        onBytes(data, len);
    });
}

void FrameCodec::setFrameCallback(FrameCallback callback) {
    m_frameCallback = std::move(callback);
}

bool FrameCodec::sendFrame(uint8_t cmd, const uint8_t* payload, size_t payloadLen) {
    // Validate total frame size
    size_t totalLen = 1 + payloadLen;  // cmd + payload
    if (totalLen > MAX_FRAME_PAYLOAD) {
        Serial.printf("FrameCodec: frame too large (%d > %d)\n", (int)totalLen, (int)MAX_FRAME_PAYLOAD);
        return false;
    }

    // Build frame buffer
    std::vector<uint8_t> frame;
    frame.reserve(totalLen);
    frame.push_back(cmd);
    
    if (payload != nullptr && payloadLen > 0) {
        frame.insert(frame.end(), payload, payload + payloadLen);
    }

    Serial.printf("FrameCodec TX: cmd 0x%02X, payload length %d\n", cmd, (int)payloadLen);
    
    return m_transport.send(frame.data(), frame.size());
}

bool FrameCodec::sendFrame(uint8_t cmd, const std::vector<uint8_t>& payload) {
    return sendFrame(cmd, payload.empty() ? nullptr : payload.data(), payload.size());
}

void FrameCodec::onBytes(const uint8_t* data, size_t len) {
    // For BLE, each notification is a complete frame
    // First byte is the command/response code
    
    if (len == 0) {
        Serial.println("FrameCodec RX: empty frame received");
        return;
    }

    if (len > MAX_FRAME_PAYLOAD) {
        Serial.printf("FrameCodec RX: frame too large (%d > %d), rejecting\n", 
                      (int)len, (int)MAX_FRAME_PAYLOAD);
        return;
    }

    uint8_t cmd = data[0];
    const uint8_t* payload = (len > 1) ? (data + 1) : nullptr;
    size_t payloadLen = (len > 1) ? (len - 1) : 0;

    Serial.printf("FrameCodec RX: cmd 0x%02X, payload length %d\n", cmd, (int)payloadLen);

    if (m_frameCallback) {
        m_frameCallback(cmd, payload, payloadLen);
    }
}
