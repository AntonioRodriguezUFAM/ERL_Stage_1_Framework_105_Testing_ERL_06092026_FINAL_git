#pragma once

#include <chrono>
#include <vector>
#include <numeric>
#include "../Includes/lynsyn.h" // Lynsyn library
#include <spdlog/spdlog.h>


// Custom definitions for LynsynSample flags (pending confirmation from usbprotocol.h)
#define SAMPLE_FLAG_VOLTAGE_VALID 0x0004 // Bit 2
#define SAMPLE_FLAG_CURRENT_VALID 0x0008 // Bit 3

/**
 * @struct PowerStats
 * @brief Represents power measurements across multiple sensors at a single point in time.
 */
// struct PowerStats {
//     std::chrono::system_clock::time_point timestamp;
//   // std::vector<double> voltages; // Volts, indexed by sensor ID
//    // std::vector<double> currents; // Amps, indexed by sensor ID

//     std::vector<double> voltages{4, 0.0}; // Default to 4 sensors
//     std::vector<double> currents{4, 0.0};
//     std::vector<double> power{4, 0.0};    // Add power vector

//     /**
//      * @brief Power for a specific sensor (P = V * I).
//      * @param i Sensor index.
//      * @return Power in watts.
//      */
//     // double sensorPower(size_t i) const {
//     //     if (i < voltages.size() && i < currents.size()) {
//     //         return voltages[i] * currents[i];
//     //     }
//     //     return 0.0;
//     // }
//     double sensorPower(size_t i) const { return i < power.size() ? power[i] : 0.0; }

//     /**
//      * @brief Calculates power for each sensor (P = V * I).
//      * @return Vector of power values in watts.
//      */
//     // std::vector<double> powerPerSensor() const {
//     //     std::vector<double> powers;
//     //     size_t count = std::min(voltages.size(), currents.size());
//     //     powers.reserve(count);
//     //     for (size_t i = 0; i < count; ++i) {
//     //         powers.push_back(voltages[i] * currents[i]);
//     //     }
//     //     return powers;
//     // }
    
//     std::vector<double> powerPerSensor() const {
//         std::vector<double> powers(power); // Copy precomputed values
//         return powers;
//     }

//     /**
//      * @brief Total power across all sensors.
//      * @return Total power in watts.
//      */
//     // double totalPower() const {
//     //     double sum = 0.0;
//     //     size_t count = std::min(voltages.size(), currents.size());
//     //     for (size_t i = 0; i < count; ++i) {
//     //         sum += voltages[i] * currents[i];
//     //     }
//     //     return sum;
//     // }
//     double totalPower() const { return std::accumulate(power.begin(), power.end(), 0.0); }

//     /**
//      * @brief Average power across all sensors.
//      * @return Average power in watts.
//      */
//     // double averagePower() const {
//     //     size_t count = std::min(voltages.size(), currents.size());
//     //     return count > 0 ? totalPower() / count : 0.0;
//     // }
    
//     double averagePower() const { return sensorCount() > 0 ? totalPower() / sensorCount() : 0.0; }

//     /**
//      * @brief Number of active sensors.
//      * @return Number of sensors with valid data.
//      */
//     // size_t sensorCount() const {
//     //     return std::min(voltages.size(), currents.size());
//     // }
//     size_t sensorCount() const { return std::min(voltages.size(), currents.size()); }

//     void setSensorData(size_t i, double voltage, double current) {
//         if (i < voltages.size()) {
//             voltages[i] = voltage;
//             currents[i] = current;
//             power[i] = voltage * current; // Update power atomically
//         }
//     }
// };

struct PowerStats {
    std::chrono::system_clock::time_point timestamp;
    std::vector<double> voltages{4, 0.0};
    std::vector<double> currents{4, 0.0};
    std::vector<double> power{4, 0.0};
    double sensorPower(size_t i) const { return i < power.size() ? power[i] : 0.0; }
    std::vector<double> powerPerSensor() const { return power; }
    double totalPower() const { return std::accumulate(power.begin(), power.end(), 0.0); }
    double averagePower() const { return sensorCount() > 0 ? totalPower() / sensorCount() : 0.0; }
    size_t sensorCount() const { return std::min(voltages.size(), currents.size()); }
    void setSensorData(size_t i, double voltage, double current) {
        if (i < voltages.size()) {
            voltages[i] = voltage;
            currents[i] = current;
            power[i] = voltage * current;
        }
    }
};

/**
 * @brief Convert raw LynsynSample to PowerStats structure.
 * @param s LynsynSample containing voltage, current, and time data.
 * @return PowerStats with populated fields.
 */
// inline PowerStats convertToPowerStats(const LynsynSample& s) {
//     PowerStats stats;

//     // Convert LynsynSample time (cycles) to timestamp
//     double seconds = lynsyn_cyclesToSeconds(s.time);
//     auto duration = std::chrono::duration<double>(seconds);
//     stats.timestamp = std::chrono::system_clock::time_point(
//         std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));

//     // Populate voltages and currents, up to LYNSYN_MAX_SENSORS or 4 (per CSV)
//     constexpr size_t max_sensors = std::min(static_cast<size_t>(LYNSYN_MAX_SENSORS), static_cast<size_t>(4));
//     stats.voltages.reserve(max_sensors);
//     stats.currents.reserve(max_sensors);

//     for (size_t i = 0; i < max_sensors; ++i) {
//         // Optionally check flags to ensure valid data (customize based on SAMPLE_FLAG_*)
//         // Example: if (s.flags & SAMPLE_FLAG_VOLTAGE_VALID) { ... }
//         stats.voltages.push_back(s.voltage[i]);
//         stats.currents.push_back(s.current[i]);
//     }

//     return stats;
// }

inline PowerStats convertToPowerStats(const LynsynSample& s) {
    PowerStats stats;
    double seconds = lynsyn_cyclesToSeconds(s.time);
    auto duration = std::chrono::duration<double>(seconds);
    stats.timestamp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));

    constexpr size_t max_sensors = std::min(static_cast<size_t>(LYNSYN_MAX_SENSORS), static_cast<size_t>(4));
    stats.voltages.resize(max_sensors, 0.0); // Pre-size with default
    stats.currents.resize(max_sensors, 0.0);

    for (size_t i = 0; i < max_sensors; ++i) {
        if (s.flags & SAMPLE_FLAG_VOLTAGE_VALID) {
            stats.voltages[i] = s.voltage[i];
        }
        if (s.flags & SAMPLE_FLAG_CURRENT_VALID) {
            stats.currents[i] = s.current[i];
        }
    }

    spdlog::debug("[convertToPowerStats] Populated {} voltages, {} currents (flags: {:04x})", 
                  stats.voltages.size(), stats.currents.size(), s.flags);
    return stats;
}