// DisplayConfig.h
// #pragma once

// #include <string>

// /**
//  * @struct DisplayConfig
//  * @brief Holds window configuration for SDL display.
//  */
// struct DisplayConfig {
//     int  width;           ///< The desired window width.
//     int  height;          ///< The desired window height.
//     bool fullScreen;      ///< Whether to display fullscreen or not.
//     std::string windowTitle; ///< The window title to be displayed.

//     // Default constructor with sensible defaults
//     DisplayConfig()
//         : width(640/2)
//         , height(480/2)
//         , fullScreen(false)
//         , windowTitle("ERL Display")
//     {
//     }
    

//     // Parameterized constructor if you want custom settings in one shot
//     DisplayConfig(int w, int h, bool fs, const std::string& title)
//         : width(w)
//         , height(h)
//         , fullScreen(fs)
//         , windowTitle(title)
//     {
//     }
// };

#pragma once
#include <string>

/**
 * @struct DisplayConfig
 * @brief Holds window configuration for SDL display.
 */
struct DisplayConfig {
    int width;           ///< The desired window width.
    int height;          ///< The desired window height.
    bool fullScreen;     ///< Whether to display fullscreen or not.
    std::string windowTitle; ///< The window title to be displayed.

    // Default constructor with sensible defaults
    DisplayConfig()
        : width(640)
        , height(480)
        , fullScreen(false)
        , windowTitle("ERL Display")
    {}
    // Parameterized constructor if you want custom settings in one shot
    
    bool validate() const {
        if (width <= 0 || height <= 0) return false;
        if (windowTitle.empty()) return false;
        return true;
    }

    // Parameterized constructor
    DisplayConfig(int w, int h, bool fs, const std::string& title)
        : width(w)
        , height(h)
        , fullScreen(fs)
        , windowTitle(title)
    {}


};