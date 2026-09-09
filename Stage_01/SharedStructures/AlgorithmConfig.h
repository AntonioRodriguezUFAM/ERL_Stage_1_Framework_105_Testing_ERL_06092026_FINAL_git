// //AlgorithmConfig.h

// /**
//  * Key Features:
// Diverse Workloads:

// GaussianBlur: Memory bandwidth and cache utilization

// MatrixMultiply: CPU/FPU performance (potential for SIMD)

// Mandelbrot: Floating-point performance

// MultiThreadedInvert: Thread scaling efficiency

// Parallelization:

// Custom parallelFor implementation for workload distribution

// Support for configurable concurrency levels

// Async operations for multi-threaded processing

// Hardware Stress Tests:

// Memory-bound operations

// CPU-bound computations

// Floating-point intensive tasks

// Concurrent execution patterns

// Metrics Tracking:

// Time per operation

// Frame processing rate

// Thread utilization efficiency

// Memory bandwidth measurements

// To use this for hardware benchmarking:

// Create different configuration profiles

// Run each algorithm type with varying parameters

// Compare performance metrics across:

// Single-threaded vs multi-threaded

// CPU vs GPU (when implemented)

// Different hardware configurations

// Various problem sizes

// This framework now supports comprehensive hardware evaluation across multiple compute paradigms.
//  * 
//  */


// /**
// updating the AlgorithmConfig structure to include an algorithm type. Adding an enum AlgorithmType with entries like Invert, Grayscale, EdgeDetection, etc.,
//  will allow switching between different processing methods.
//  */


// /**
//  * @enum AlgorithmType
//  * @brief Example enumeration for choosing different algorithm processing types.
//  */

// //AlgorithmConfig.h


// #pragma once
// #include <string>
// #include <cstdint>
// #include "OpticalFlowConfig.h"  
// #include "../Stage_01/Concretes/CudaUtiles.h" // For cudaAvailable


// namespace hrl {

// enum class AlgorithmType {
//     Invert,
//     Grayscale,
//     EdgeDetection,
//     GaussianBlur,
//     MatrixMultiply,
//     Mandelbrot,
//     PasswordHash,
//     MultiPipeline,
//     GPUMatrixMultiply,
//     MultiThreadedInvert,
//     OpticalFlow_LucasKanade,
//     // new types GPU-based filters
//     SobelEdge,
//     MedianFilter,
//     HistogramEqualization,
//     HeterogeneousGaussianBlur
// };



// // =============================================================================
// // Helper Functions
// // =============================================================================

// // =============================================================================
// // String to AlgorithmType Converter (Updated)
// // =============================================================================
// inline std::string algorithmTypeToString(hrl::AlgorithmType type) {
//     switch (type) {
//         case hrl::AlgorithmType::Invert:                    return "Invert";
//         case hrl::AlgorithmType::Grayscale:                 return "Grayscale";
//         case hrl::AlgorithmType::EdgeDetection:             return "EdgeDetection";
//         case hrl::AlgorithmType::GaussianBlur:              return "GaussianBlur";
//         case hrl::AlgorithmType::SobelEdge:                 return "SobelEdge";
//         case hrl::AlgorithmType::MedianFilter:              return "MedianFilter";
//         case hrl::AlgorithmType::HistogramEqualization:     return "HistogramEqualization";
//         case hrl::AlgorithmType::HeterogeneousGaussianBlur: return "HeterogeneousGaussianBlur";
//         case hrl::AlgorithmType::OpticalFlow_LucasKanade:   return "OpticalFlow_LK";
//         case hrl::AlgorithmType::MatrixMultiply:            return "MatrixMultiply";
//         case hrl::AlgorithmType::Mandelbrot:                 return "Mandelbrot";
//         case hrl::AlgorithmType::PasswordHash:              return "PasswordHash";
//         case hrl::AlgorithmType::MultiPipeline:             return "MultiPipeline";
//         case hrl::AlgorithmType::GPUMatrixMultiply:         return "GPUMatrixMultiply";
//         case hrl::AlgorithmType::MultiThreadedInvert:       return "MultiThreadedInvert";
//         default:                                            return "Unknown";
//     }
// }

// // =============================================================================
// // Improved String to AlgorithmType Converter
// // =============================================================================
// inline hrl::AlgorithmType stringToAlgorithmType(const std::string& name) {
//     static const std::map<std::string, hrl::AlgorithmType> kMap = {
//         {"Invert",                    hrl::AlgorithmType::Invert},
//         {"Grayscale",                 hrl::AlgorithmType::Grayscale},
//         {"EdgeDetection",             hrl::AlgorithmType::EdgeDetection},
//         {"GaussianBlur",              hrl::AlgorithmType::GaussianBlur},
//         {"SobelEdge",                 hrl::AlgorithmType::SobelEdge},
//         {"MedianFilter",              hrl::AlgorithmType::MedianFilter},
//         {"HistogramEqualization",     hrl::AlgorithmType::HistogramEqualization},
//         {"HeterogeneousGaussianBlur", hrl::AlgorithmType::HeterogeneousGaussianBlur},
//         {"OpticalFlow_LucasKanade",   hrl::AlgorithmType::OpticalFlow_LucasKanade},
//         {"OpticalFlow_LK",            hrl::AlgorithmType::OpticalFlow_LucasKanade},
//         {"MatrixMultiply",            hrl::AlgorithmType::MatrixMultiply},
//         {"Mandelbrot",                 hrl::AlgorithmType::Mandelbrot},
//         {"PasswordHash",              hrl::AlgorithmType::PasswordHash},
//         {"MultiPipeline",             hrl::AlgorithmType::MultiPipeline},
//         {"GPUMatrixMultiply",         hrl::AlgorithmType::GPUMatrixMultiply},
//         {"MultiThreadedInvert",       hrl::AlgorithmType::MultiThreadedInvert}, 
//         // Case insensitive aliases
//         {"invert",                    hrl::AlgorithmType::Invert},
//         {"sobeledge",                 hrl::AlgorithmType::SobelEdge},
//         {"histogram",                 hrl::AlgorithmType::HistogramEqualization},
//         {"opticalflow_lk",            hrl::AlgorithmType::OpticalFlow_LucasKanade},
//         {"opticalflow_lucaskanade",   hrl::AlgorithmType::OpticalFlow_LucasKanade},
//         {"matrixMultiply",            hrl::AlgorithmType::MatrixMultiply},
//         {"mandelbrot",                 hrl::AlgorithmType::Mandelbrot},
//         {"passwordhash",              hrl::AlgorithmType::PasswordHash},
//         {"multipipeline",             hrl::AlgorithmType::MultiPipeline},
//         {"gpumatrixmultiply",         hrl::AlgorithmType::GPUMatrixMultiply},
//         {"multithreadedinvert",       hrl::AlgorithmType::MultiThreadedInvert}
//     };

//     // Exact match
//     auto it = kMap.find(name);
//     if (it != kMap.end()) {
//         return it->second;
//     }

//     // Case-insensitive match
//     std::string lower = name;
//     std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

//     //for (const auto& [key, value] : kMap) {
//     for (const auto& kv : kMap) {
//         const auto& key = kv.first;
//         const auto& value = kv.second;
//         std::string klower = key;
//         std::transform(klower.begin(), klower.end(), klower.begin(), ::tolower);
//         if (klower == lower) {
//             spdlog::info("[CONFIG] Matched algorithm '{}' ? {}", name, algorithmTypeToString(value));
//             return value;
//         }
//     }

//     // Fallback with warning
//     spdlog::warn("[CONFIG] Unknown algorithm '{}'. Falling back to SobelEdge (recommended for heterogeneous testing).", name);
//     return AlgorithmType::SobelEdge;   // Better default than Invert for PhD experiments
// }





// // =============================================================================
// // AlgorithmConfig Structure
// // =============================================================================
// struct AlgorithmConfig {
//     int32_t concurrencyLevel = 4;      // Default: reasonable for Jetson Nano // Number of threads
//     AlgorithmType algorithmType = AlgorithmType::SobelEdge;  // Better default for testing
//     std::string modelPath;
//     int32_t matrixSize = 512;
//     int32_t mandelbrotIter = 100;
//     int32_t blurRadius = 5;
//     int32_t medianWindowSize = 5;
//     bool useGPU = true;                     // Default to heterogeneous algorithms for benchmarking purposes
//     OpticalFlowConfig opticalFlowConfig;

//     // ===================================================================
//     // Validation with PhD-friendly logging
//     // ===================================================================
//     bool validate() const {
//         bool valid = true;

//         if (concurrencyLevel <= 0) {
//             spdlog::error("[CONFIG] Invalid concurrencyLevel: {}", concurrencyLevel);
//             valid = false;
//         }
//         if (matrixSize <= 0) {
//             spdlog::error("[CONFIG] Invalid matrixSize: {}", matrixSize);
//             valid = false;
//         }
//         if (mandelbrotIter <= 0) {
//             spdlog::error("[CONFIG] Invalid mandelbrotIter: {}", mandelbrotIter);
//             valid = false;
//         }
//         if (blurRadius <= 0) {
//             spdlog::error("[CONFIG] Invalid blurRadius: {}", blurRadius);
//             valid = false;
//         }
//         if (medianWindowSize <= 0) {
//             spdlog::error("[CONFIG] Invalid medianWindowSize: {}", medianWindowSize);
//             valid = false;
//         }

//         // Heterogeneous Computing Check
//         if (useGPU) {
//             if (!hrl::isCudaAvailable()) {
//                 spdlog::warn("[CONFIG] GPU requested but CUDA not available. Falling back to CPU.");
//                 // Do NOT fail validation - just log (important for PhD ablation studies)
//                 // const_cast<AlgorithmConfig*>(this)->useGPU = false; // Optional auto-disable
//             } else {
//                 spdlog::info("[CONFIG] GPU acceleration enabled for {}");// ,/* algorithm name if you add a helper */);
//             }
//         } else {
//             spdlog::info("[CONFIG] Running on CPU only (Heterogeneous disabled)");
//         }

//         if (!opticalFlowConfig.validate()) {
//             spdlog::error("[CONFIG] Invalid OpticalFlowConfig");
//             valid = false;
//         }

//         return valid;
//     }

//     // bool validate() const {
//     //     if (concurrencyLevel <= 0) return false;
//     //     if (matrixSize <= 0) return false;
//     //     if (mandelbrotIter <= 0) return false;
//     //     if (blurRadius <= 0) return false;
//     //     if (medianWindowSize <= 0) return false;
//     //     if (useGPU) {
//     //         // Placeholder: Check CUDA availability
//     //         if (!cudaAvailable()) return false;
//     //     }
//     //     return opticalFlowConfig.validate();
//     // }


//     // Helper for logging
//     std::string toString() const {
//         return "AlgorithmConfig[Type=" + algorithmTypeToString(algorithmType) +
//                ", GPU=" + (useGPU ? "ON" : "OFF") +
//                ", Concurrency=" + std::to_string(concurrencyLevel) + "]";
//     }
// };

// } // namespace hrl


#pragma once
#include <string>
#include <cstdint>
#include <map>
#include <algorithm>
#include "OpticalFlowConfig.h"  
#include "../Stage_01/Concretes/CudaUtiles.h" // For cudaAvailable


namespace hrl {

enum class AlgorithmType {
    Invert,
    Grayscale,
    EdgeDetection,
    GaussianBlur,
    MatrixMultiply,
    Mandelbrot,
    PasswordHash,
    MultiPipeline,
    GPUMatrixMultiply,
    MultiThreadedInvert,
    OpticalFlow_LucasKanade,
    // new types GPU-based filters
    SobelEdge,
    MedianFilter,
    HistogramEqualization,
    HeterogeneousGaussianBlur
};



// =============================================================================
// Helper Functions
// =============================================================================

// =============================================================================
// String to AlgorithmType Converter (Updated)
// =============================================================================
inline std::string algorithmTypeToString(hrl::AlgorithmType type) {
    switch (type) {
        case hrl::AlgorithmType::Invert:                    return "Invert";
        case hrl::AlgorithmType::Grayscale:                 return "Grayscale";
        case hrl::AlgorithmType::EdgeDetection:             return "EdgeDetection";
        case hrl::AlgorithmType::GaussianBlur:              return "GaussianBlur";
        case hrl::AlgorithmType::SobelEdge:                 return "SobelEdge";
        case hrl::AlgorithmType::MedianFilter:              return "MedianFilter";
        case hrl::AlgorithmType::HistogramEqualization:     return "HistogramEqualization";
        case hrl::AlgorithmType::HeterogeneousGaussianBlur: return "HeterogeneousGaussianBlur";
        case hrl::AlgorithmType::OpticalFlow_LucasKanade:   return "OpticalFlow_LK";
        case hrl::AlgorithmType::MatrixMultiply:            return "MatrixMultiply";
        case hrl::AlgorithmType::Mandelbrot:                 return "Mandelbrot";
        case hrl::AlgorithmType::PasswordHash:              return "PasswordHash";
        case hrl::AlgorithmType::MultiPipeline:             return "MultiPipeline";
        case hrl::AlgorithmType::GPUMatrixMultiply:         return "GPUMatrixMultiply";
        case hrl::AlgorithmType::MultiThreadedInvert:       return "MultiThreadedInvert";
        default:                                            return "Unknown";
    }
}

// =============================================================================
// Improved String to AlgorithmType Converter
// =============================================================================
inline hrl::AlgorithmType stringToAlgorithmType(const std::string& name) {
    static const std::map<std::string, hrl::AlgorithmType> kMap = {
        {"Invert",                    hrl::AlgorithmType::Invert},
        {"Grayscale",                 hrl::AlgorithmType::Grayscale},
        {"EdgeDetection",             hrl::AlgorithmType::EdgeDetection},
        {"GaussianBlur",              hrl::AlgorithmType::GaussianBlur},
        {"SobelEdge",                 hrl::AlgorithmType::SobelEdge},
        {"MedianFilter",              hrl::AlgorithmType::MedianFilter},
        {"HistogramEqualization",     hrl::AlgorithmType::HistogramEqualization},
        {"HeterogeneousGaussianBlur", hrl::AlgorithmType::HeterogeneousGaussianBlur},
        {"OpticalFlow_LucasKanade",   hrl::AlgorithmType::OpticalFlow_LucasKanade},
        {"OpticalFlow_LK",            hrl::AlgorithmType::OpticalFlow_LucasKanade},
        {"MatrixMultiply",            hrl::AlgorithmType::MatrixMultiply},
        {"Mandelbrot",                 hrl::AlgorithmType::Mandelbrot},
        {"PasswordHash",              hrl::AlgorithmType::PasswordHash},
        {"MultiPipeline",             hrl::AlgorithmType::MultiPipeline},
        {"GPUMatrixMultiply",         hrl::AlgorithmType::GPUMatrixMultiply},
        {"MultiThreadedInvert",       hrl::AlgorithmType::MultiThreadedInvert}, 
        // Case insensitive aliases
        {"invert",                    hrl::AlgorithmType::Invert},
        {"sobeledge",                 hrl::AlgorithmType::SobelEdge},
        {"histogram",                 hrl::AlgorithmType::HistogramEqualization},
        {"opticalflow_lk",            hrl::AlgorithmType::OpticalFlow_LucasKanade},
        {"opticalflow_lucaskanade",   hrl::AlgorithmType::OpticalFlow_LucasKanade},
        {"matrixMultiply",            hrl::AlgorithmType::MatrixMultiply},
        {"mandelbrot",                 hrl::AlgorithmType::Mandelbrot},
        {"passwordhash",              hrl::AlgorithmType::PasswordHash},
        {"multipipeline",             hrl::AlgorithmType::MultiPipeline},
        {"gpumatrixmultiply",         hrl::AlgorithmType::GPUMatrixMultiply},
        {"multithreadedinvert",       hrl::AlgorithmType::MultiThreadedInvert}
    };

    // Exact match
    auto it = kMap.find(name);
    if (it != kMap.end()) {
        return it->second;
    }

    // Case-insensitive match
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& kv : kMap) {
        const auto& key = kv.first;
        const auto& value = kv.second;
        std::string klower = key;
        std::transform(klower.begin(), klower.end(), klower.begin(), ::tolower);
        if (klower == lower) {
            spdlog::info("[CONFIG] Matched algorithm '{}' -> {}", name, algorithmTypeToString(value));
            return value;
        }
    }

    // Fallback with warning
    spdlog::warn("[CONFIG] Unknown algorithm '{}'. Falling back to SobelEdge (recommended for heterogeneous testing).", name);
    return AlgorithmType::SobelEdge;   // Better default than Invert for PhD experiments
}





// =============================================================================
// AlgorithmConfig Structure
// =============================================================================
struct AlgorithmConfig {
    int32_t concurrencyLevel = 4;      // Default: reasonable for Jetson Nano // Number of threads
    AlgorithmType algorithmType = AlgorithmType::SobelEdge;  // Better default for testing
    std::string modelPath;
    int32_t matrixSize = 512;
    int32_t mandelbrotIter = 100;
    int32_t blurRadius = 5;
    int32_t medianWindowSize = 5;
    bool useGPU = true;                     // Default to heterogeneous algorithms for benchmarking purposes
    OpticalFlowConfig opticalFlowConfig;

    // ===================================================================
    // Validation with PhD-friendly logging
    // ===================================================================
    bool validate() const {
        bool valid = true;

        if (concurrencyLevel <= 0) {
            spdlog::error("[CONFIG] Invalid concurrencyLevel: {}", concurrencyLevel);
            valid = false;
        }
        if (matrixSize <= 0) {
            spdlog::error("[CONFIG] Invalid matrixSize: {}", matrixSize);
            valid = false;
        }
        if (mandelbrotIter <= 0) {
            spdlog::error("[CONFIG] Invalid mandelbrotIter: {}", mandelbrotIter);
            valid = false;
        }
        if (blurRadius <= 0) {
            spdlog::error("[CONFIG] Invalid blurRadius: {}", blurRadius);
            valid = false;
        }
        if (medianWindowSize <= 0) {
            spdlog::error("[CONFIG] Invalid medianWindowSize: {}", medianWindowSize);
            valid = false;
        }

        // Heterogeneous Computing Check
        if (useGPU) {
            if (!hrl::isCudaAvailable()) {
                spdlog::warn("[CONFIG] GPU requested but CUDA not available. Falling back to CPU.");
                // Do NOT fail validation - just log (important for PhD ablation studies)
            } else {
                spdlog::info("[CONFIG] GPU acceleration enabled for {}", algorithmTypeToString(algorithmType));
            }
        } else {
            spdlog::info("[CONFIG] Running on CPU only (Heterogeneous disabled)");
        }

        if (!opticalFlowConfig.validate()) {
            spdlog::error("[CONFIG] Invalid OpticalFlowConfig");
            valid = false;
        }

        return valid;
    }

    // Helper for logging
    std::string toString() const {
        return "AlgorithmConfig[Type=" + algorithmTypeToString(algorithmType) +
               ", GPU=" + (useGPU ? "ON" : "OFF") +
               ", Concurrency=" + std::to_string(concurrencyLevel) + "]";
    }
};

} // namespace hrl