#ifndef SYSTEM_CAPTURE_CONFIG_H
#define SYSTEM_CAPTURE_CONFIG_H

#include <string>

struct SystemCaptureConfig {
    int samplingRate;
    int duration;
    std::string outputFilePath; // Optional: Path to save captured data

    // Default constructor
    SystemCaptureConfig()
        : samplingRate(100),
        duration(60),
        outputFilePath("captured_data.txt") {}
};

#endif // SYSTEM_CAPTURE_CONFIG_H