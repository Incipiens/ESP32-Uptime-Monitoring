#pragma once

#include "IByteTransport.hpp"
#include <vector>
#include <functional>
#include <cstdint>

/**
 * FrameCodec - Framing layer for MeshCore protocol
 * 
 * This class handles:
 * - Parsing incoming byte streams into frames
 * - Building TX frames from cmd + payload
 * 
 * For BLE: Each notification/write is a complete frame (no length prefix needed)
 * The first byte is the command/response code, followed by payload.
 * 
 * Frame format: [cmd (1 byte)] [payload (variable)]
 * 
 * Note: The MeshCore BLE protocol uses raw frames without length prefix
 * because BLE notifications are already length-delimited.
 */
class FrameCodec {
public:
    // Maximum frame payload size (MeshCore limit)
    static constexpr size_t MAX_FRAME_PAYLOAD = 172;
    
    // Callback for received complete frames
    using FrameCallback = std::function<void(uint8_t cmd, const uint8_t* payload, size_t payloadLen)>;

    /**
     * Construct FrameCodec with a byte transport
     * @param transport The underlying byte transport
     */
    explicit FrameCodec(IByteTransport& transport);

    /**
     * Set callback for received frames
     * @param callback Function to call when a complete frame is received
     */
    void setFrameCallback(FrameCallback callback);

    /**
     * Send a frame with command and payload
     * @param cmd Command byte
     * @param payload Payload data (can be empty)
     * @param payloadLen Length of payload
     * @return true if frame was sent successfully
     */
    bool sendFrame(uint8_t cmd, const uint8_t* payload = nullptr, size_t payloadLen = 0);

    /**
     * Send a frame with command and vector payload
     * @param cmd Command byte
     * @param payload Payload data vector
     * @return true if frame was sent successfully
     */
    bool sendFrame(uint8_t cmd, const std::vector<uint8_t>& payload);

    /**
     * Process incoming bytes from transport
     * For BLE, each call to this method should be a complete frame
     * @param data Pointer to received data
     * @param len Length of received data
     */
    void onBytes(const uint8_t* data, size_t len);

private:
    IByteTransport& m_transport;
    FrameCallback m_frameCallback;
};
