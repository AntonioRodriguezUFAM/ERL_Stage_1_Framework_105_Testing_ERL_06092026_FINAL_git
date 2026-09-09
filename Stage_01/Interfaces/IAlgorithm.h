
//IAlgorithm.h

#pragma once

#include <memory> // Add this at the top of the file
#include <functional>
#include <string>
#include <tuple>


#include "../SharedStructures/FrameData.h"
#include "../SharedStructures/AlgorithmConfig.h"

/**
 * @brief Interface for an algorithm that can be started and stopped.
 * 
 * This interface allows the orchestrator or other components
 * to control the lifecycle of an algorithm. The algorithm may
 * run in one or more internal threads (e.g., for frame processing).
 * 
 * Key Features:
 * 1. Lifecycle Control: `startAlgorithm` and `stopAlgorithm` methods to manage the algorithm's runtime behavior.
 * 2. Metrics Retrieval: Methods for retrieving performance metrics (e.g., FPS, processing time).
 * 3. Configuration: Accepts an `AlgorithmConfig` object for advanced settings.
 * 4. Error Handling: Provides a callback mechanism to report errors or warnings.
 */


namespace hrl {

  enum class AlgorithmType;   // Forward declare if needed

class IAlgorithm {
public:
    virtual ~IAlgorithm() = default;

    /**
     * @brief Start the algorithm’s processing (e.g., spawn threads).
     */
    virtual void startAlgorithm() = 0;

    /**
     * @brief Stop the algorithm’s processing (e.g., join threads).
     */
    virtual void stopAlgorithm() = 0;

    /**
     * @brief Process a single frame synchronously.
     * @param frame The frame to be processed.
     * @return True if processed successfully; false otherwise.
     */
    //virtual bool processFrame(const FrameData& inputFrame, FrameData& outputFrame) = 0;

    /**
     * @brief Process a single frame synchronously.
     * @param ZeroCopyframe The frame to be processed.
     * @return True if processed successfully; false otherwise.
     */
   virtual bool processFrameZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& inputFrame, std::shared_ptr<ZeroCopyFrameData>& outputFrame)=0;

        /**
     * @brief Configure the algorithm with advanced settings.
     * @param config A struct holding various algorithm-related configurations.
     * @return True if configuration was successful; false otherwise.
     */
    virtual bool configure(const AlgorithmConfig& config) = 0;

    /**
     * @brief Set a callback for reporting errors or warnings during algorithm execution.
     * @param callback A function that will be called with an error message.
     */
    virtual void setErrorCallback(std::function<void(const std::string&)>) = 0;

    /**
     * @brief Retrieve the current frames-per-second (FPS) metric.
     * @return The current FPS as a double.
     */
    virtual double getFps() const = 0;

    /**
     * @brief Retrieve the average processing time for frames.
     * @return The average processing time in milliseconds as a double.
     */
    virtual double getAverageProcTime() const = 0;

     // Add this if you want to retrieve the processed buffer
    virtual const uint8_t* getProcessedBuffer() const = 0;
    // or a more elaborate function, e.g.:
    // virtual void getProcessedFrame(std::vector<uint8_t>& out) const = 0;

    virtual  double getLastFPS() const = 0;
    
    // Get algorithm metrics: FPS and average processing time
    virtual std::tuple<double, double> getAlgorithmMetrics() const = 0;


    // ---------------------- Dynamic Runtime Algorithm Switching ----------------------
    virtual void setAlgorithmType(AlgorithmType newType)= 0;



    std::shared_ptr<IAlgorithm> algo_; // Shared ownership of the algorithm implementation
};

}