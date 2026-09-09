// IDisplay.h
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "../SharedStructures/DisplayConfig.h"

/**
 * @brief Interface for a display component capable of rendering frames.
 */
class IDisplay {
public:
    virtual ~IDisplay() = default;

    /**
     * @brief Configures the display with advanced settings.
     * @param config The DisplayConfig struct containing display parameters.
     * @return True if configuration was successful; false otherwise.
     */
    virtual bool configure(const DisplayConfig& config) = 0;

    /**
     * @brief Initializes the display with the specified width and height.
     * @param width The width of the display window.
     * @param height The height of the display window.
     * @return True if initialization was successful; false otherwise.
     */
    virtual bool initializeDisplay(int width, int height) = 0;

    /**
     * @brief Updates the "original" frame with new RGB data.
     * @param rgbData Pointer to the RGB data.
     * @param width The width of the frame.
     * @param height The height of the frame.
     */
    virtual void updateOriginalFrame(const uint8_t* rgbData, int width, int height) = 0;

    /**
     * @brief Updates the "processed" frame with new RGB data.
     * @param rgbData Pointer to the RGB data.
     * @param width The width of the frame.
     * @param height The height of the frame.
     */
    virtual void updateProcessedFrame(const uint8_t* rgbData, int width, int height) = 0;

    /**
     * @brief Renders the frames and polls for events (e.g., window close).
     */
    virtual void renderAndPollEvents() = 0;

    /**
     * @brief Sets a callback for reporting errors or warnings.
     * @param callback A function that will be called with an error message.
     */
    virtual void setErrorCallback(std::function<void(const std::string&)> callback) = 0;

    /**
     * @brief Closes the display and releases all resources.
     */
    virtual void closeDisplay() = 0;

    /**
     * @brief Checks if the display is still running.
     * @return True if the display is running; false otherwise.
     */
    // virtual bool isRunning() const = 0;

    
};