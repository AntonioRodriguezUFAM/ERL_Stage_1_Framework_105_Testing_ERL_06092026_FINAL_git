// AlgorithmConcreteKernels.cuh
//
// Declares persistent GPU resource management and CUDA kernel launchers.
// Using persistent device buffers, a dedicated CUDA stream, and pinned host
// memory eliminates per-frame cudaMalloc/cudaFree overhead and enables async
// H2D/D2H transfers that overlap with CPU work.

#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <cuda_runtime.h>

// Forward declaration to avoid including the full header
class ZeroCopyFrameData;

namespace AlgorithmConcreteKernels {

// ============================================================================
// CudaResources
//
// Holds all per-algorithm-instance GPU state.  Allocate once via
// initCudaResources() when the algorithm starts and release via
// destroyCudaResources() when it stops.  All launchers accept a reference to
// this struct and reuse its buffers rather than calling cudaMalloc/cudaFree
// on every frame.
// ============================================================================
struct CudaResources {
    uint8_t*      d_in          = nullptr;  // Device input buffer  (dataSize bytes)
    uint8_t*      d_out         = nullptr;  // Device output buffer (dataSize bytes)
    uint8_t*      d_tmp         = nullptr;  // Temp buffer for two-pass Gaussian
    unsigned int* d_hist        = nullptr;  // Histogram (256 uints)
    int*          d_cdf         = nullptr;  // CDF prefix-sum (256 ints)
    float*        d_kernel      = nullptr;  // Gaussian 1D kernel (maxKernelElems floats)
    uint8_t*      h_pinned      = nullptr;  // Page-locked host output buffer
    size_t        capacity      = 0;        // Current allocated dataSize
    int           kernelElems   = 0;        // Current allocated kernel length
    cudaStream_t  stream        = nullptr;  // Dedicated CUDA stream for async ops
};

// Allocate/initialise all device and pinned-host buffers for frames of
// dataSize bytes, with a Gaussian kernel of at most maxKernelElems floats.
// Safe to call again when dataSize/maxKernelElems grows (reallocates).
// Throws std::runtime_error on CUDA failure.
void initCudaResources(CudaResources& res, size_t dataSize, int maxKernelElems = 31);

// Release all device and pinned-host buffers and destroy the stream.
// Safe to call on a zero-initialised struct.
void destroyCudaResources(CudaResources& res);

// ---- Kernel launchers (reuse pre-allocated CudaResources) ----

void launchSobelEdgeKernel(
    CudaResources& res,
    const std::shared_ptr<ZeroCopyFrameData>& frame,
    std::vector<uint8_t>& processedBuffer);

void launchMedianFilterKernel(
    CudaResources& res,
    const std::shared_ptr<ZeroCopyFrameData>& frame,
    std::vector<uint8_t>& processedBuffer,
    int windowSize /* 3 or 5 */);

void launchHistogramEqualizationKernel(
    CudaResources& res,
    const std::shared_ptr<ZeroCopyFrameData>& frame,
    std::vector<uint8_t>& processedBuffer);

void launchHeterogeneousGaussianBlurKernel(
    CudaResources& res,
    const std::shared_ptr<ZeroCopyFrameData>& frame,
    std::vector<uint8_t>& processedBuffer,
    int radius,
    double gpuSplit = 1.0);   // [Micro-scheduler] fraction of rows processed on GPU, [0,1]

// 
// Launches a two-pass Gaussian blur kernel that applies a 1D kernel horizontally

void launchMandelbrotKernel(
    CudaResources& res,
    const std::shared_ptr<ZeroCopyFrameData>& frame,
    std::vector<uint8_t>& processedBuffer);

} // namespace AlgorithmConcreteKernels
