#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

/**
 * IByteTransport - Interface for byte-oriented transport layer
 * 
 * This interface abstracts the underlying transport mechanism (BLE, USB, etc.)
 * and provides a simple byte-oriented API for higher layers.
 */
class IByteTransport {
public:
    // Callback for received bytes from the transport
    using RxCallback = std::function<void(const uint8_t* data, size_t len)>;
    
    // Callback for transport state changes
    using StateCallback = std::function<void(bool connected)>;

    virtual ~IByteTransport() = default;

    /**
     * Send raw bytes over the transport
     * @param data Pointer to data buffer
     * @param len Length of data to send
     * @return true if send was successful
     */
    virtual bool send(const uint8_t* data, size_t len) = 0;

    /**
     * Check if transport is connected
     * @return true if connected
     */
    virtual bool isConnected() const = 0;

    /**
     * Set callback for received bytes
     * @param callback Function to call when bytes are received
     */
    virtual void setRxCallback(RxCallback callback) = 0;

    /**
     * Set callback for connection state changes
     * @param callback Function to call when connection state changes
     */
    virtual void setStateCallback(StateCallback callback) = 0;
};
