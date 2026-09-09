// FrameData.h, 

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Data structure representing a single frame's metadata and content.
 * FrameData: A simple struct containing pointers and metadata about a single frame.
 * 
 * This struct is commonly used to pass frames from a data source
 * (e.g., camera) to an algorithm or other consumer.
 * 
 * Key Points
 * 1. data: Typically points to an mmap’ed buffer or a driver buffer.
 * 2. size: The number of bytes in the buffer.
 * 3. width / height: Helps algorithms know the frame dimensions (can be optional if you get that from elsewhere).
 * 4. timestamp: Useful for synchronization or measuring latency; the source depends on your driver or system clock.
 * 
 * 
 * Explanation
* 1. New fields: pixelFormat, frameNumber, isKeyFrame can help advanced workflows (e.g., compressed video).
* 2. Ownership: Clarify in your documentation or code whether data must be freed by the consumer, or reused by the producer after queueBuffer().

 */
struct FrameData {
    /**
     * @brief Pointer to the frame buffer data in memory.
     * 
     * Ownership of this pointer is not implied. The underlying memory
     * might be managed by a camera driver (e.g., via mmap).
     */
    //void* dataVec = nullptr;
    std::vector<uint8_t> dataVec; // Holds the actual frame data

    /**
     * @brief The total size of the frame data in bytes.
     */
    size_t size = 0;

    /**
     * @brief Width of the frame in pixels (if known).
     */
    int width = 0;

    /**
     * @brief Height of the frame in pixels (if known).
     */
    int height = 0;

    /**
     * @brief Timestamp for the frame, typically in microseconds or a similar unit.
     */
    uint64_t timestamp = 0;

     /**
     * @brief Optional string to describe the pixel format, e.g. "YUV420".
     */
    std::string pixelFormat;

    /**
     * @brief A sequential number for this frame, if needed (e.g., for debugging).
     */
    uint64_t frameNumber = 0;

    /**
     * @brief Whether this frame is a keyframe (useful for compressed formats).
     */
    bool isKeyFrame = false;

    /**
     * 
     * 
     */
    size_t bufferIndex;  // Added buffer index tracking

};



//==========================================


