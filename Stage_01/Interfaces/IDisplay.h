// IDisplay.h


/*Step 1: Define IDisplay Interface (IDisplay.h)*/
// IDisplay.h

#pragma once
#include <cstdint>
#include <functional>
#include "../SharedStructures/DisplayConfig.h"

/**
 * @brief Interface for an SDL-like display component.
 */
class IDisplay {
public:
    virtual ~IDisplay() = default;

     /**
     * @brief Configure the display monitoring with advanced settings.
     * @param config The DisplayConfig struct with desired monitoring parameters.
     * @return True if configuration was successful; false otherwise.
     */
    virtual bool configure(const DisplayConfig& config) = 0;

    /**
     * @brief Initialize the display with a given width/height for the window.
     * @return True if successful; false otherwise.
     */
    virtual bool initializeDisplay(int width, int height) = 0;
    

    /**
     * @brief Update the "original" frame (e.g., unprocessed) with new RGB data.
     */
    //virtual void updateOriginalFrame(const uint8_t* rgbData, int width, int /*height*/) = 0;
    
    virtual void updateOriginalFrame(const uint8_t* rgbData, int width, [[maybe_unused]] int height) = 0;
   


    /**
     * @brief Update the "processed" frame (e.g., post-alg) with new RGB data.
     */
    //virtual void updateProcessedFrame(const uint8_t* rgbData, int width, int /*height*/) = 0;
    virtual void updateProcessedFrame(const uint8_t* rgbData, int width, [[maybe_unused]] int height) = 0;

    /**
     * @brief Perform rendering (show both frames) and poll any SDL events.
     */
    virtual void renderAndPollEvents() = 0;

     /**
     * @brief Set a callback for reporting errors or warnings during monitoring.
     * @param callback A function that will be called with an error message.
     */
    virtual void setErrorCallback(std::function<void(const std::string&)>) = 0;


    // Possibly more methods..
    virtual void closeDisplay()  = 0;

      /**
     * @brief Checks if the display is still running.
     * @return True if the display is running; false otherwise.
     */
     virtual bool is_Running() = 0;
};
