// ISoC.h
// #pragma once

// #include <functional>
// #include <string>
// #include <memory>
// #include <thread>
// #include <atomic>
// #include <cstdio>
// #include <array>
// #include <stdexcept>
// #include <mutex>
// #include "../SharedStructures/SoCConfig.h"
// #include "../SharedStructures/jetsonNanoInfo.h" // Ensure the struct JetsonNanoInfo type is visible

// //#include "Stage_01/Concretes/SoCConcrete.h"

// //s#include "../SharedStructures/jetsonNanoInfo.h" // Ensure the type is visible


// /**
//  * @brief Interface for SoC (System on Chip) monitoring or control.
//  */
// class ISoC {
// public:
//     //virtual ISoC() = default;
//     // In ISoC.h
//     virtual ~ISoC() = default;


//     /**
//      * @brief Initialize SoC-related features or start monitoring threads.
//      */
//     virtual void initializeSoC() = 0;

//     /**
//      * @brief Retrieve SoC specifications (CPU/GPU info, memory, etc.).
//      * @return A string describing the SoC hardware specs.
//      */
//     virtual std::string getSpecs() = 0;

//     /**
//      * @brief Get the latest performance metrics, such as CPU usage, GPU usage, and temperatures.
//      * @return A `JetsonNanoInfo` struct containing performance metrics.
//      */
//     virtual JetsonNanoInfo getPerformance() const = 0;


//     /**
//      * @brief Stop SoC monitoring or shutdown any related tasks.
//      */
//     virtual void stopSoC() = 0;

//     /**
//      * @brief Configure the SoC monitoring with advanced settings.
//      * @param config The SoCConfig struct with desired monitoring parameters.
//      * @return True if configuration was successful; false otherwise.
//      */
//     virtual bool configure(const SoCConfig& config) = 0;

//     /**
//      * @brief Set a callback for reporting errors or warnings during monitoring.
//      * @param callback A function that will be called with an error message.
//      */
//     virtual void setErrorCallback(std::function<void(const std::string&)>) = 0;

//     /**
//      *@brief Push SoC stats to the logger
//      *@param stats The SoC stats to be pushed to the logger
//      */
//     virtual void pushSoCStats(const JetsonNanoInfo& stats) = 0;


// };


//======================================================================

// ISoC.h

#pragma once

#include <functional>
#include <string>
#include <memory>


#include <thread>
#include <atomic>
#include <cstdio>
#include <array>
#include <stdexcept>
#include <mutex>


#include "../SharedStructures/SoCConfig.h"
//#include "../SharedStructures/jetsonNanoInfo.h"
#include "../SharedStructures/allModulesStatcs.h" // Ensure the struct JetsonNanoInfo type is visible

/**
 * @class ISoC
 * @brief Interface (pure virtual class) for monitoring or controlling a System on Chip.
 *
 * Typical usage involves:
 *   1. Calling configure(...) to set up SoC settings (poll intervals, board type, custom command, etc.).
 *   2. Calling initializeSoC() to start any background threads or processes that read SoC data.
 *   3. Optionally calling getPerformance() to retrieve the latest performance metrics (CPU usage, temps, etc.).
 *   4. Relying on pushSoCStats(...) if a push-based design is desired to publish new stats to an external aggregator.
 *   5. Finally calling stopSoC() to stop all SoC monitoring and clean up resources.
 */
class ISoC {
public:
    virtual ~ISoC() = default;

    /**
     * @brief Configure the SoC module with advanced settings (poll intervals, custom commands, CSV export, etc.).
     * @param config The SoCConfig structure containing these settings.
     * @return True if configuration was successful, false otherwise.
     */
    virtual bool configure(const SoCConfig& config) = 0;

    /**
     * @brief Initialize SoC-related features or start monitoring threads.
     *
     * Typically spawns a background thread that reads a system utility (e.g. 'tegrastats')
     * or other data source, parsing it into SoC metrics.
     */
    virtual bool initializeSoC() = 0;

    /**
     * @brief Retrieve SoC specifications (CPU/GPU info, memory, etc.).
     * @return A string describing the SoC hardware.
     *
     * This may be static (e.g., "Jetson Nano") or determined at runtime.
     */
    virtual std::string getSpecs() = 0;

    /**
     * @brief Retrieve the latest performance metrics, such as CPU usage, GPU usage, memory usage, and temperatures.
     * @return A `JetsonNanoInfo` struct containing performance metrics.
     *
     * Note: If you are using a push-based design only, this may be optional.
     */
    virtual JetsonNanoInfo getPerformance() const = 0;

    /**
     * @brief Push SoC stats to an external logger or aggregator.
     * @param stats The SoC stats to be pushed.
     *
     * In a push-based design, the SoC monitoring class calls this internally
     * when new data is available. Alternatively, external code could also call this
     * if it obtains SoC data in some other manner.
     */
    virtual void pushSoCStats(const JetsonNanoInfo& stats) = 0;

    /**
     * @brief Stop SoC monitoring or shut down any related tasks.
     *
     * Typically signals the background thread to stop reading system metrics
     * and joins the thread before returning. Also closes any resources like pipes.
     */
    virtual void stopSoC() = 0;

    /**
     * @brief Set an optional callback for reporting errors or warnings during SoC monitoring.
     * @param callback A function that will be called with an error message.
     *
     * If no callback is provided, SoC implementations can default to logging (e.g. spdlog) or std::cerr.
     * 
     */

     virtual void setErrorCallback(std::function<void(const std::string&)>) =0;
    

};
