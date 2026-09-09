//IData.h
#pragma once

#include <functional>
#include <string>
// Instead of struct CameraConfig { ... }, just include the header:
#include "../SharedStructures/CameraConfig.h"
//#include "../SharedStructures/jetsonNanoInfo.h" // Ensure the struct JetsonNanoInfo type is visible
#include "../SharedStructures/allModulesStatcs.h"


/**
 * @brief Interface for a data source, typically a camera or similar device.
 * IData: Abstract camera/data source interface (open, stream, retrieve frames).
 * IData defines minimal responsibilities for opening a device,
 * starting/stopping streaming, and dequeuing/queuing frames.
 * 
 * Key Points
 * 1. openDevice(): Allows you to specify which device to open (e.g., /dev/video0).
 * 2 .startStreaming()/stopStreaming(): Controls the streaming state.
 * 3. dequeFrame() / queueBuffer(): Provides minimal V4L2-like semantics for frame retrieval and buffer reuse.
 * 
 * Explanation
 * 1. CameraConfig: A struct capturing advanced camera parameters.
 * 2. configure(...): Allows dynamic changes to resolution, format, or other device settings.
 * 3. setErrorCallback(...): Notifies external code when something goes wrong (e.g., device errors).
 * 4. add and use an isStreaming() method so that you only spawn capture threads for cameras that have successfully started streaming.

 */

typedef std::function<void(const std::string&)> ErrorCallback; // Type alias

class IData {
public:
   // virtual ~IData() = default;
    virtual ~IData() = default; // Essential: Virtual destructor defined!
    
    /**
     * @brief Opens the underlying data device (e.g., /dev/video0 for a acamer).
     * @param path The path to the data source (camera).
     * @return True if successfully opened; false otherwise.
     */
    virtual bool openDevice(const std::string& path) = 0;
   
   // Additional device management methods
   virtual void closeDevice () = 0; // Close the device
   virtual void resetDevice () =0; // Reset the device


    /**
     * @brief Starts streaming from the device.
     * @return True if successfully started; false otherwise.
     */
    virtual bool startStreaming() = 0;

    /**
     * @brief Stops streaming from the device.
     * @return True if successfully stopped; false otherwise.
     */
    virtual bool stopStreaming() = 0;


    // << Newly added method >>
    virtual bool isStreaming() const = 0;

    /**
     * @brief Dequeues a single frame from the device.
     * @param dataPtr [out] Pointer to the memory for the frame data.
     * @param sizeBytes [out] The size of the data in bytes.
     * @return True if a frame was successfully dequeued; false otherwise.
     */
    //virtual bool dequeFrame(void*& dataPtr, size_t& sizeBytes) = 0;
    //virtual bool dequeFrame(void*& dataPtr, size_t& sizeBytes, size_t& bufferIndex) = 0;
    virtual bool dequeFrame()= 0;


    /**
     * @brief Re-queues the last dequeued buffer so the device can reuse it.
     * @return True if successfully queued; false otherwise.
     */
    //virtual bool queueBuffer() = 0;
    virtual bool queueBuffer(size_t bufferIndex) = 0;

    /**
     * @brief Configure the device with advanced settings, e.g. resolution, FPS, etc.
     * @param config A struct holding camera/device-specific parameters.
     * @return True if configuration was successful; false otherwise.
     */
    virtual bool configure(const CameraConfig& config) = 0;

    /**
     * @brief Set a callback for reporting errors or warnings during device operations.
     * @param callback A function that will be called with an error message.
     */
    virtual void setErrorCallback(std::function<void(const std::string&)>) = 0;
    

    
    /**
    * @brief Retrieves the last calculated frames per second (FPS) of the streaming device.
    * @return The last calculated FPS as a double.
    */
    virtual double getLastFPS()  = 0;
    


    /**
    * @brief Gets the current size of the queue holding frames.
    * @return The number of frames currently in the queue.
    */
    virtual int getQueueSize() const = 0;

    /**
     * @brief Push the Camera Metrics
     * @return 
     */
    virtual void pushCameraMetrics()=0;


    /**
     * @brief start camera capture
     * @return true 
     * @return false 
     */
       
       virtual bool startCapture()=0 ;  // Starts internal capture thread
    /**
     * @brief Stop camera capture and cleanup.
     * 
     * @return true 
     * @return false 
     */

        virtual bool stopCapture()=0;   // Stops thread and ensures cleanup

// Modified IData interface



};
