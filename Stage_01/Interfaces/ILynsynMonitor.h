
// ILynsynMonitor.h

#pragma once

#include <functional>
#include <string>
#include <iostream>
#include "../SharedStructures/LynsynMonitorConfig.h"


/**
 * @class ILynsynMonitor
 * @brief Interface for a module that uses the Lynsyn hardware to measure
 *        current and voltage on the Jetson (or other boards).
 */
class ILynsynMonitor {
public:
    virtual ~ILynsynMonitor() = default;

    /**
     * @brief Configure the Lynsyn module with advanced settings.
     * @param config The LynsynMonitorConfig structure with poll intervals, duration, breakpoints, etc.
     * @return True if configuration was successful, false otherwise.
     */
    virtual bool configure(const LynsynMonitorConfig& config) = 0;
    

    /**
     * @brief Initialize Lynsyn, open the device, etc. After this call,
     *        monitoring is *not* started yet – just prepares resources.
     * @return True on success, false on failure.
     */
    virtual bool initialize() = 0;

    /**
     * @brief Start the actual sampling (spawns a thread or triggers sampling).
     *        The new samples will be read in the background until stop() is called
     *        or the configured duration runs out.
     */
    virtual void startMonitoring() = 0;

    /**
     * @brief Stop the sampling process and tear down resources (closes device).
     */
    virtual void stop() = 0;

    /**
     * @brief Provide a callback that is called whenever a new sample is read. 
     * @param callback A function taking a LynsynSample or some struct with current/voltage data.
     *        The user can store it or log it as desired.
     */
    virtual void setSampleCallback(std::function<void(const struct LynsynSample&)> callback) = 0;

    /**
     * @brief (Optional) Set an error callback for reporting warnings/errors internally.
     */
    virtual void setErrorCallback(std::function<void(const std::string&)>) = 0;
};
