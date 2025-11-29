#pragma once

/**
 * MeshCore - Layered implementation of MeshCore Companion Radio Protocol
 * 
 * Architecture:
 * 
 *   [Application Layer]
 *           |
 *   [CompanionProtocol] - MeshCore protocol logic, state machine, API
 *           |
 *   [FrameCodec]        - Frame parsing and building
 *           |
 *   [BLECentralTransport : IByteTransport] - BLE connection and I/O
 * 
 * Usage:
 *   1. Create BLECentralTransport with configuration
 *   2. Create FrameCodec with the transport
 *   3. Create CompanionProtocol with both
 *   4. Initialize BLE, connect, start session
 *   5. Find channel and send messages
 */

#include "IByteTransport.hpp"
#include "BLECentralTransport.hpp"
#include "FrameCodec.hpp"
#include "CompanionProtocol.hpp"
