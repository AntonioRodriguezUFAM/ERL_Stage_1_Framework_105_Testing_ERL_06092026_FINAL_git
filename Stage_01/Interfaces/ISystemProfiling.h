// ISystemProfiling.h

/*
The ISystemProfiling interface acts as an abstract contract that defines the communication protocol 
between SystemCaptureFactory (metrics publisher) and SystemProfilingFactory (metrics consumer). 
By implementing this interface, we decouple the factories, enabling modular and testable design.
*/

// Defines the contract for accessing metrics
#pragma once
#include "../SharedStructures/jetsonNanoInfo.h"  // Ensure JetsonNanoInfo is visible
#include <tuple>

/**
 * @brief The ISystemProfiling interface acts as an abstract contract that defines 
 *        the communication protocol between SystemCaptureFactory (metrics publisher) 
 *        and SystemProfilingFactory (metrics consumer).
 *
 * Purpose: Acts as a contract between SystemCaptureFactory (metrics publisher) and SystemProfilingFactory (metrics consumer).
 * 
 * Key Methods:
 *  getSoCMetrics(): Retrieves SoC metrics (CPU/GPU usage, temperature, etc.).
 *  getCameraMetrics(int cameraIndex): Retrieves camera-specific metrics (FPS, queue size).
 *  getAlgorithmMetrics(): Retrieves algorithm metrics (FPS, average processing time).
 * 
 * By implementing this interface, we decouple the factories, enabling a modular
 * and testable design. 
 */
class ISystemProfiling {
public:
    virtual ~ISystemProfiling() = default;

    // Get SoC metrics: CPU usage, GPU usage, CPU temp, GPU temp
    virtual JetsonNanoInfo getSoCMetrics() const = 0;

    // Get camera metrics: FPS and queue size for a specific camera
    virtual std::tuple<double, int> getCameraMetrics(int cameraIndex) const = 0;


    // Get algorithm metrics: FPS and average processing time
    virtual std::tuple<double, double> getAlgorithmMetrics() const = 0;
};
