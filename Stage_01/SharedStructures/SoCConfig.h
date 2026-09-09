//SoCConfig.h
#pragma once
#include <string>

/**
 * @enum BoardType
 * @brief Identifies which board or SoC we are monitoring (Jetson Nano, TX2, Xavier, etc.)
 */
enum class BoardType {
    JetsonNano,
    JetsonTX2,
    JetsonXavier,
    RaspberryPi,
    Unknown
};


// For a 500ms Sampling Rate:

/**
 * @struct SoCConfig
 * @brief Configuration for SoC monitoring.
 */
struct SoCConfig {
    /**
     * @brief Poll interval in milliseconds for reading tegrastats output.
     *        Default 500 = read once per second.
     */
    int32_t pollIntervalMs = 500;

    /**
     * @brief The command used to run the SoC monitoring tool (by default "tegrastats").
     *        E.g., "tegrastats --interval 500"
     */
    
    std::string customCommand ="tegrastats --interval 500";
    

    /**
     * @brief If true, SoC data is exported to CSV on each parse.
     */
    bool exportCSV = true;

    /**
     * @brief Path to CSV file for SoC data if exportCSV == true.
     */
    std::string csvPath = "jetson_nano_tegrastats.csv";

    /**
     * @brief The board type we are running on (JetsonNano, TX2, Xavier, etc.).
     *        This can let you choose different parse logic if needed.
     */
    BoardType boardType = BoardType::JetsonNano;

    /**
     * @brief If true, attempt partial parse (log partial successes).
     *        If false, treat parse failures as errors.
     */
    bool partialParseAllowed = true;

    /**
     * @brief If true, use the new parsing logic (if available).
     *        If false, use the old parsing logic.
     */
    bool validate() const {
        if (pollIntervalMs <= 0) return false;
        if (customCommand.empty()) return false;
        if (exportCSV && csvPath.empty()) return false;
        // Optional: Check if parent directory of csvPath exists
        if (exportCSV && !csvPath.empty()) {
            std::string dir = csvPath.substr(0, csvPath.find_last_of("/\\"));
            if (!dir.empty()) {
                std::ifstream f(dir + "/.test");
                if (!f.good()) return false;
            }
        }
        return true;
    }
};
