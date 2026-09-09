//===============================================================
// AlgorithmConcreteKernels.cu
//
// Key improvements over original implementation:
//  - Persistent device buffers allocated once per algorithm lifecycle
//    (no per-frame cudaMalloc/cudaFree).
//  - Page-locked (pinned) host output buffer for faster D2H transfers.
//  - Dedicated CUDA stream: H2D copy, kernel, D2H copy all queued async
//    and synchronised only once at the end of each frame.
//  - GPU prefix-sum for histogram CDF  eliminates D2H/H2D round-trip.
//  - CUDA errors throw std::runtime_error; callers fall back to CPU path.

#include "AlgorithmConcreteKernels.cuh"
#include "../SharedStructures/ZeroCopyFrameData.h"

#include <cuda_runtime.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <vector>
#include <stdexcept>
#include <cmath>
#include <algorithm>

// --- ADD after existing includes ---
#include "CudaUtiles.h"          // brings in the extern declaration

// Provide the ONE definition the linker needs:
// namespace hrl {
//     std::atomic<bool> cudaAvailable{false};
// }

// =============================== CUDA error helper ===============================
// Throws std::runtime_error on failure so callers can catch and fall back to CPU.
static inline void checkCuda(cudaError_t result,
                             char const* const func,
                             char const* const file,
                             int const line) {
    if (result != cudaSuccess) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
                      "CUDA error at %s:%d code=%u(%s) \"%s\"",
                      file, line,
                      static_cast<unsigned int>(result),
                      cudaGetErrorName(result),
                      func);
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}
#define checkCudaErrors(val) checkCuda((val), #val, __FILE__, __LINE__)

// =============================== device helpers =================================
__device__ __forceinline__ int d_clamp_i(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

__device__ __forceinline__ uint8_t d_sat_u8(int x) {
    return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
}

// ================================================================================
// KERNELS (YUYV: 2 bytes/pixel, Y at even byte)
// ================================================================================

__global__ void sobelEdgeKernel(const uint8_t* input, uint8_t* output, int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int ystride = width * 2;
    const int idx = y * ystride + x * 2;

    if (x > 0 && x < width - 1 && y > 0 && y < height - 1) {
        const int gx =
            -int(input[idx - 2 - ystride]) + int(input[idx + 2 - ystride]) +
            -2 * int(input[idx - 2])       + 2 * int(input[idx + 2]) +
            -int(input[idx - 2 + ystride]) + int(input[idx + 2 + ystride]);

        const int gy =
            -int(input[idx - 2 - ystride]) - 2 * int(input[idx - ystride]) - int(input[idx + 2 - ystride]) +
             int(input[idx - 2 + ystride]) + 2 * int(input[idx + ystride]) + int(input[idx + 2 + ystride]);

        const float magf = sqrtf(float(gx * gx + gy * gy));
        const int   mag  = d_clamp_i(int(magf), 0, 255);

        output[idx]     = static_cast<uint8_t>(mag);
        output[idx + 1] = 128;
    } else {
        output[idx]     = input[idx];
        output[idx + 1] = input[idx + 1];
    }
}

__global__ void medianFilterKernel(const uint8_t* input, uint8_t* output,
                                   int width, int height, int windowSize) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int radius  = windowSize / 2;
    const int ystride = width * 2;
    const int idx     = y * ystride + x * 2;

    if (windowSize > 5) return;

    uint8_t window[25];
    int count = 0;

    for (int j = -radius; j <= radius; ++j) {
        const int yy = (y + j < 0) ? 0 : ((y + j >= height) ? (height - 1) : (y + j));
        for (int i = -radius; i <= radius; ++i) {
            const int xx = (x + i < 0) ? 0 : ((x + i >= width) ? (width - 1) : (x + i));
            window[count++] = input[yy * ystride + xx * 2];
        }
    }

    for (int i = 0; i < count - 1; ++i) {
        for (int j = 0; j < count - i - 1; ++j) {
            if (window[j] > window[j + 1]) {
                const uint8_t t = window[j];
                window[j] = window[j + 1];
                window[j + 1] = t;
            }
        }
    }

    output[idx]     = window[count / 2];
    output[idx + 1] = 128;
}

__global__ void histogramKernel(const uint8_t* input, unsigned int* hist,
                                int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int idx = (y * width + x) * 2;
    atomicAdd(&hist[input[idx]], 1);
}

// GPU exclusive prefix-sum (scan) on the 256-element histogram to produce CDF.
// Runs as a single block of 256 threads using shared memory.
__global__ void histogramCdfKernel(const unsigned int* hist, int* cdf, int* minCdfOut) {
    __shared__ int sdata[256];
    const int tid = threadIdx.x; // 0..255

    sdata[tid] = static_cast<int>(hist[tid]);
    __syncthreads();

    // Inclusive prefix sum (Blelloch-style simple variant for 256 elements)
    for (int stride = 1; stride < 256; stride <<= 1) {
        int val = 0;
        if (tid >= stride) val = sdata[tid - stride];
        __syncthreads();
        sdata[tid] += val;
        __syncthreads();
    }

    cdf[tid] = sdata[tid];

    // Write minCdf: smallest non-zero CDF value (i.e. count of the rarest
    // brightness level present).  If somehow no pixels exist, use 0 so the
    // remap denominator becomes (totalPixels - 0) = totalPixels and the output
    // is left at its natural range.
    if (tid == 0) {
        int mn = 0; // fallback: no valid minimum found  equalization is a no-op
        for (int i = 0; i < 256; ++i) {
            if (sdata[i] > 0) { mn = sdata[i]; break; }
        }
        *minCdfOut = mn;
    }
}

__global__ void remapKernel(const uint8_t* input, uint8_t* output,
                            int width, int height,
                            const int* cdf, int minCdf) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int idx = (y * width + x) * 2;
    const int totalPixels = width * height;

    const int c = cdf[input[idx]];
    const int denom = totalPixels - minCdf;
    uint8_t newY = 0;
    if (denom > 0) {
        const float norm = float(c - minCdf) / float(denom);
        newY = d_sat_u8(int(norm * 255.0f));
    }

    output[idx]     = newY;
    output[idx + 1] = 128;
}

__global__ void gaussianBlurHorizontalKernel(const uint8_t* input, uint8_t* output,
                                             int width, int height,
                                             int radius, const float* kernel) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int ystride = width * 2;
    float acc = 0.0f;
    for (int k = -radius; k <= radius; ++k) {
        int xx = x + k;
        xx = (xx < 0) ? 0 : ((xx >= width) ? (width - 1) : xx);
        acc += float(input[y * ystride + xx * 2]) * kernel[k + radius];
    }

    const int outIdx = y * ystride + x * 2;
    output[outIdx]     = d_sat_u8(int(acc));
    output[outIdx + 1] = 128;
}

__global__ void gaussianBlurVerticalKernel(const uint8_t* input, uint8_t* output,
                                           int width, int height,
                                           int radius, const float* kernel) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int ystride = width * 2;
    float acc = 0.0f;
    for (int k = -radius; k <= radius; ++k) {
        int yy = y + k;
        yy = (yy < 0) ? 0 : ((yy >= height) ? (height - 1) : yy);
        acc += float(input[yy * ystride + x * 2]) * kernel[k + radius];
    }

    const int outIdx = y * ystride + x * 2;
    output[outIdx]     = d_sat_u8(int(acc));
    output[outIdx + 1] = 128;
}

__global__ void invertYUYVKernel(const unsigned char* in,
                                 unsigned char* out,
                                 int width, int height)
{
    int x2 = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    int y  =  blockIdx.y * blockDim.y + threadIdx.y;
    if (x2 + 1 >= width || y >= height) return;

    int idx = y * width * 2 + x2 * 2;
    out[idx]     = 255 - in[idx];
    out[idx + 1] = in[idx + 1];
    out[idx + 2] = 255 - in[idx + 2];
    out[idx + 3] = in[idx + 3];
}

// ============================================================================
// Mandelbrot Kernel (Pure Compute Stress Test)
// ============================================================================
__global__ void mandelbrotKernel(uint8_t* d_out, int width, int height, int max_iter) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x < width && y < height) {
        // Map pixel coordinates to the Mandelbrot complex plane
        // X: [-2.0, 1.0], Y: [-1.5, 1.5]
        float cx = (x * 3.0f / (float)width) - 2.0f;
        float cy = (y * 3.0f / (float)height) - 1.5f;

        float zx = 0.0f;
        float zy = 0.0f;
        int iter = 0;

        while (zx * zx + zy * zy <= 4.0f && iter < max_iter) {
            float tmp = zx * zx - zy * zy + cx;
            zy = 2.0f * zx * zy + cy;
            zx = tmp;
            iter++;
        }

        // Map iterations to 0-255 grayscale pixel
        uint8_t pixelValue = (iter == max_iter) ? 0 : (uint8_t)((iter * 255) / max_iter);
        d_out[y * width + x] = pixelValue;
    }
}

// ================================================================================
// Resource management
// ================================================================================
namespace AlgorithmConcreteKernels {

void initCudaResources(CudaResources& res, size_t dataSize, int maxKernelElems) {
    if (dataSize == 0) {
        spdlog::warn("[CudaResources] initCudaResources called with dataSize=0; skipping");
        return;
    }

    // Reallocate only if capacity needs to grow
    if (dataSize > res.capacity) {
        if (res.d_in)     { checkCudaErrors(cudaFree(res.d_in));     res.d_in  = nullptr; }
        if (res.d_out)    { checkCudaErrors(cudaFree(res.d_out));    res.d_out = nullptr; }
        if (res.d_tmp)    { checkCudaErrors(cudaFree(res.d_tmp));    res.d_tmp = nullptr; }
        if (res.h_pinned) { checkCudaErrors(cudaFreeHost(res.h_pinned)); res.h_pinned = nullptr; }

        checkCudaErrors(cudaMalloc(&res.d_in,     dataSize));
        checkCudaErrors(cudaMalloc(&res.d_out,    dataSize));
        checkCudaErrors(cudaMalloc(&res.d_tmp,    dataSize));
        checkCudaErrors(cudaMallocHost(reinterpret_cast<void**>(&res.h_pinned), dataSize));
        res.capacity = dataSize;
        spdlog::info("[CudaResources] Allocated device/pinned buffers: {} bytes", dataSize);
    }

    // Fixed-size histogram and CDF buffers (always 256 entries)
    if (!res.d_hist) {
        checkCudaErrors(cudaMalloc(&res.d_hist, 256 * sizeof(unsigned int)));
        checkCudaErrors(cudaMalloc(&res.d_cdf,  257 * sizeof(int))); // +1 for minCdf slot
        spdlog::info("[CudaResources] Allocated histogram/CDF buffers");
    }

    // Gaussian kernel buffer
    if (maxKernelElems > res.kernelElems) {
        if (res.d_kernel) { checkCudaErrors(cudaFree(res.d_kernel)); res.d_kernel = nullptr; }
        checkCudaErrors(cudaMalloc(&res.d_kernel, maxKernelElems * sizeof(float)));
        res.kernelElems = maxKernelElems;
    }

    // Create stream once
    if (!res.stream) {
        checkCudaErrors(cudaStreamCreate(&res.stream));
        spdlog::info("[CudaResources] CUDA stream created");
    }
}

// void destroyCudaResources(CudaResources& res) {
//     if (res.stream)   { cudaStreamDestroy(res.stream);      res.stream   = nullptr; }
//     if (res.d_in)     { cudaFree(res.d_in);                 res.d_in     = nullptr; }
//     if (res.d_out)    { cudaFree(res.d_out);                res.d_out    = nullptr; }
//     if (res.d_tmp)    { cudaFree(res.d_tmp);                res.d_tmp    = nullptr; }
//     if (res.d_hist)   { cudaFree(res.d_hist);               res.d_hist   = nullptr; }
//     if (res.d_cdf)    { cudaFree(res.d_cdf);                res.d_cdf    = nullptr; }
//     if (res.d_kernel) { cudaFree(res.d_kernel);             res.d_kernel = nullptr; }
//     if (res.h_pinned) { cudaFreeHost(res.h_pinned);         res.h_pinned = nullptr; }
//     res.capacity    = 0;
//     res.kernelElems = 0;
//     spdlog::info("[CudaResources] All GPU resources released");
// }

void destroyCudaResources(CudaResources& res) {
    if (res.stream)   { cudaStreamDestroy(res.stream);      res.stream   = nullptr; }
    if (res.d_in)     { cudaFree(res.d_in);                 res.d_in     = nullptr; }
    if (res.d_out)    { cudaFree(res.d_out);                res.d_out    = nullptr; }
    if (res.d_tmp)    { cudaFree(res.d_tmp);                res.d_tmp    = nullptr; }
    if (res.d_hist)   { cudaFree(res.d_hist);               res.d_hist   = nullptr; }
    if (res.d_cdf)    { cudaFree(res.d_cdf);                res.d_cdf    = nullptr; }
    if (res.d_kernel) { cudaFree(res.d_kernel);             res.d_kernel = nullptr; }
    if (res.h_pinned) { cudaFreeHost(res.h_pinned);         res.h_pinned = nullptr; }
    
    res.capacity = 0;
    res.kernelElems = 0;
    spdlog::info("[CudaResources] Destroyed CUDA resources successfully");
}

// ================================================================================
// LAUNCHER WRAPPERS  reuse CudaResources; async H2D + kernel + D2H on stream
// ================================================================================

void launchSobelEdgeKernel(CudaResources& res,
                           const std::shared_ptr<ZeroCopyFrameData>& frame,
                           std::vector<uint8_t>& processedBuffer) {
    if (!frame || !frame->dataPtr) {
        spdlog::warn("[CUDA Sobel] Invalid frame/dataPtr");
        return;
    }

    const int    width    = frame->width;
    const int    height   = frame->height;
    const size_t dataSize = frame->size;

    initCudaResources(res, dataSize);

    checkCudaErrors(cudaMemcpyAsync(res.d_in, frame->dataPtr, dataSize,
                                   cudaMemcpyHostToDevice, res.stream));

    dim3 block(16, 16);
    dim3 grid((width + 15) / 16, (height + 15) / 16);
    sobelEdgeKernel<<<grid, block, 0, res.stream>>>(res.d_in, res.d_out, width, height);
    checkCudaErrors(cudaGetLastError());

    processedBuffer.resize(dataSize);
    checkCudaErrors(cudaMemcpyAsync(res.h_pinned, res.d_out, dataSize,
                                   cudaMemcpyDeviceToHost, res.stream));
    checkCudaErrors(cudaStreamSynchronize(res.stream));
    std::copy(res.h_pinned, res.h_pinned + dataSize, processedBuffer.begin());
}

void launchMedianFilterKernel(CudaResources& res,
                              const std::shared_ptr<ZeroCopyFrameData>& frame,
                              std::vector<uint8_t>& processedBuffer,
                              int windowSize) {
    if (!frame || !frame->dataPtr) {
        spdlog::warn("[CUDA Median] Invalid frame/dataPtr");
        return;
    }
    if (windowSize != 3 && windowSize != 5) {
        //spdlog::error("[CUDA Median] Only 3x3 or 5x5 windows are supported (got {}).", windowSize);
        throw std::invalid_argument(
            "CUDA MedianFilter supports only window sizes 3 and 5");
        //return;
    }

    const int    width    = frame->width;
    const int    height   = frame->height;
    const size_t dataSize = frame->size;

    initCudaResources(res, dataSize);

    checkCudaErrors(cudaMemcpyAsync(res.d_in, frame->dataPtr, dataSize,
                                   cudaMemcpyHostToDevice, res.stream));

    dim3 block(16, 16);
    dim3 grid((width + 15) / 16, (height + 15) / 16);
    medianFilterKernel<<<grid, block, 0, res.stream>>>(res.d_in, res.d_out, width, height, windowSize);
    checkCudaErrors(cudaGetLastError());

    processedBuffer.resize(dataSize);
    checkCudaErrors(cudaMemcpyAsync(res.h_pinned, res.d_out, dataSize,
                                   cudaMemcpyDeviceToHost, res.stream));
    checkCudaErrors(cudaStreamSynchronize(res.stream));
    std::copy(res.h_pinned, res.h_pinned + dataSize, processedBuffer.begin());
}

//=========================================================================================

void launchHistogramEqualizationKernel(CudaResources& res,
                                       const std::shared_ptr<ZeroCopyFrameData>& frame,
                                       std::vector<uint8_t>& processedBuffer) {
    if (!frame || !frame->dataPtr) {
        spdlog::warn("[CUDA HistEq] Invalid frame/dataPtr");
        return;
    }

    const int    width    = frame->width;
    const int    height   = frame->height;
    const size_t dataSize = frame->size;

    initCudaResources(res, dataSize);
    
    // d_cdf is allocated as 257 ints; last slot used for minCdf
    int* d_minCdf = res.d_cdf + 256;

    

    checkCudaErrors(cudaMemcpyAsync(res.d_in, frame->dataPtr, dataSize,
                                   cudaMemcpyHostToDevice, res.stream));
    checkCudaErrors(cudaMemsetAsync(res.d_hist, 0, 256 * sizeof(unsigned int), res.stream));

    dim3 block(16, 16);
    dim3 grid((width + 15) / 16, (height + 15) / 16);

    // 1) Build histogram of Y channel
    histogramKernel<<<grid, block, 0, res.stream>>>(res.d_in, res.d_hist, width, height);
    checkCudaErrors(cudaGetLastError());

    // 2) Compute CDF entirely on GPU (single block of 256 threads, shared memory scan)
    histogramCdfKernel<<<1, 256, 0, res.stream>>>(res.d_hist, res.d_cdf, d_minCdf);
    checkCudaErrors(cudaGetLastError());

    // 3) Retrieve minCdf (single int) and remap
    int minCdf = 0;
    checkCudaErrors(cudaMemcpyAsync(&minCdf, d_minCdf, sizeof(int),
                                   cudaMemcpyDeviceToHost, res.stream));
    checkCudaErrors(cudaStreamSynchronize(res.stream));

    remapKernel<<<grid, block, 0, res.stream>>>(res.d_in, res.d_out, width, height, res.d_cdf, minCdf);
    checkCudaErrors(cudaGetLastError());

    processedBuffer.resize(dataSize);
    checkCudaErrors(cudaMemcpyAsync(res.h_pinned, res.d_out, dataSize,
                                   cudaMemcpyDeviceToHost, res.stream));
    checkCudaErrors(cudaStreamSynchronize(res.stream));
    std::copy(res.h_pinned, res.h_pinned + dataSize, processedBuffer.begin());
}

// ================================================================================
// [Micro-scheduler] CPU-side separable Gaussian blur for a row range [rowStart,rowEnd).
// Uses the SAME normalised kernel and sigma as the GPU path so the two halves of
// the frame are visually seamless at the split boundary. Reads from the original
// input (with clamped halo), writes only its owned rows into `out`.
// ================================================================================
static void cpuGaussianBlurRows(const uint8_t* in, uint8_t* out,
                                int width, int height, int radius,
                                const std::vector<float>& kernel,
                                int rowStart, int rowEnd) {
    // Two-pass separable blur, YUYV layout (2 bytes/pixel, luma at even indices).
    std::vector<float> tmp(static_cast<size_t>(width) * (rowEnd - rowStart));
    // Horizontal pass into tmp (float), for the owned rows.
    for (int y = rowStart; y < rowEnd; ++y) {
        for (int x = 0; x < width; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int xx = x + k; if (xx < 0) xx = 0; if (xx >= width) xx = width - 1;
                acc += in[(static_cast<size_t>(y) * width + xx) * 2] * kernel[k + radius];
            }
            tmp[static_cast<size_t>(y - rowStart) * width + x] = acc;
        }
    }
    // Vertical pass from tmp; halo rows outside [rowStart,rowEnd) are read from `in`
    // directly (re-filtered horizontally on the fly) to keep the boundary correct.
    auto hAt = [&](int yy, int xx) -> float {
        if (yy >= rowStart && yy < rowEnd)
            return tmp[static_cast<size_t>(yy - rowStart) * width + xx];
        float acc = 0.0f;
        for (int k = -radius; k <= radius; ++k) {
            int x2 = xx + k; if (x2 < 0) x2 = 0; if (x2 >= width) x2 = width - 1;
            acc += in[(static_cast<size_t>(yy) * width + x2) * 2] * kernel[k + radius];
        }
        return acc;
    };
    for (int y = rowStart; y < rowEnd; ++y) {
        for (int x = 0; x < width; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int yy = y + k; if (yy < 0) yy = 0; if (yy >= height) yy = height - 1;
                acc += hAt(yy, x) * kernel[k + radius];
            }
            int v = static_cast<int>(acc + 0.5f);
            if (v < 0) v = 0; if (v > 255) v = 255;
            const size_t o = (static_cast<size_t>(y) * width + x) * 2;
            out[o]     = static_cast<uint8_t>(v);
            out[o + 1] = 128;   // neutral chroma
        }
    }
}

void launchHeterogeneousGaussianBlurKernel(CudaResources& res,
                                           const std::shared_ptr<ZeroCopyFrameData>& frame,
                                           std::vector<uint8_t>& processedBuffer,
                                           int radius,
                                           double gpuSplit) {
    if (!frame || !frame->dataPtr) {
        spdlog::warn("[CUDA HGB] Invalid frame/dataPtr");
        return;
    }
    if (radius <= 0) {
        spdlog::warn("[CUDA HGB] Non-positive radius {}; passthrough.", radius);
        processedBuffer.assign(static_cast<const uint8_t*>(frame->dataPtr),
                               static_cast<const uint8_t*>(frame->dataPtr) + frame->size);
        return;
    }

    const int    width    = frame->width;
    const int    height   = frame->height;
    const size_t dataSize = frame->size;
    const int    ksize    = radius * 2 + 1;

    // [Micro-scheduler] Partition rows between GPU and CPU by the continuous split.
    if (gpuSplit < 0.0) gpuSplit = 0.0;
    if (gpuSplit > 1.0) gpuSplit = 1.0;
    int gpuRows = static_cast<int>(gpuSplit * height + 0.5);
    if (gpuRows < 0) gpuRows = 0;
    if (gpuRows > height) gpuRows = height;
    const int cpuRows = height - gpuRows;

    processedBuffer.resize(dataSize);
    const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);

    // Build the shared normalised 1D Gaussian once (used by both CPU and GPU).
    std::vector<float> h_kernel(ksize);
    const float sigma = (radius <= 1) ? 1.0f : (float(radius) * 0.5f);
    float sum = 0.0f;
    for (int i = 0; i < ksize; ++i) {
        const float x = float(i - radius);
        h_kernel[i] = expf(-(x * x) / (2.0f * sigma * sigma));
        sum += h_kernel[i];
    }
    for (int i = 0; i < ksize; ++i) h_kernel[i] /= sum;

    // -------- GPU half: issue ALL device work asynchronously (no sync yet) -------
    // The full frame is uploaded so the vertical pass near the boundary has its
    // halo; only the GPU-owned rows [0,gpuRows) are copied back.
    bool gpuIssued = false;
    if (gpuRows > 0) {
        initCudaResources(res, dataSize, std::max(ksize, res.kernelElems));
        checkCudaErrors(cudaMemcpyAsync(res.d_in, frame->dataPtr, dataSize,
                                        cudaMemcpyHostToDevice, res.stream));
        checkCudaErrors(cudaMemcpyAsync(res.d_kernel, h_kernel.data(), ksize * sizeof(float),
                                        cudaMemcpyHostToDevice, res.stream));
        dim3 block(16, 16);
        // Grid covers only the GPU-owned rows (+1 block for boundary halo).
        dim3 grid((width + 15) / 16, (gpuRows + 15) / 16);
        gaussianBlurHorizontalKernel<<<grid, block, 0, res.stream>>>(
            res.d_in, res.d_tmp, width, height, radius, res.d_kernel);
        checkCudaErrors(cudaGetLastError());
        gaussianBlurVerticalKernel<<<grid, block, 0, res.stream>>>(
            res.d_tmp, res.d_out, width, height, radius, res.d_kernel);
        checkCudaErrors(cudaGetLastError());
        // Async copy back ONLY the GPU-owned rows.
        const size_t gpuBytes = static_cast<size_t>(gpuRows) * width * 2;
        checkCudaErrors(cudaMemcpyAsync(res.h_pinned, res.d_out, gpuBytes,
                                        cudaMemcpyDeviceToHost, res.stream));
        gpuIssued = true;
    }

    // -------- CPU half: runs on the host CONCURRENTLY with the GPU stream --------
    if (cpuRows > 0) {
        cpuGaussianBlurRows(in, processedBuffer.data(), width, height, radius,
                            h_kernel, gpuRows, height);
    }

    // -------- Join: wait for the GPU, then merge its rows into the output --------
    if (gpuIssued) {
        checkCudaErrors(cudaStreamSynchronize(res.stream));
        const size_t gpuBytes = static_cast<size_t>(gpuRows) * width * 2;
        std::copy(res.h_pinned, res.h_pinned + gpuBytes, processedBuffer.begin());
    }
}


// ================================================================================
// LAUNCHER WRAPPERS  Mandelbrot Kernel (Pure Compute Stress Test)
// ================================================================================


void launchMandelbrotKernel(
    CudaResources& res,
    const std::shared_ptr<ZeroCopyFrameData>& frame,
    std::vector<uint8_t>& processedBuffer) 
{
    const int width = frame->width;
    const int height = frame->height;
    const size_t dataSize = width * height;
    const int max_iter = 100; // Adjust for more/less compute load

    // Setup grid and block
    dim3 block(16, 16);
    dim3 grid((width + 15) / 16, (height + 15) / 16);

    // Launch kernel (No H2D copy needed because we are generating the image from math!)
    mandelbrotKernel<<<grid, block, 0, res.stream>>>(res.d_out, width, height, max_iter);
    checkCuda(cudaGetLastError(), "launchMandelbrotKernel", __FILE__, __LINE__);

    // Async copy result back to pinned host memory
    checkCuda(cudaMemcpyAsync(processedBuffer.data(), res.d_out, dataSize,
                              cudaMemcpyDeviceToHost, res.stream),
              "launchMandelbrotKernel D2H", __FILE__, __LINE__);
    
    checkCuda(cudaStreamSynchronize(res.stream), "launchMandelbrotKernel sync", __FILE__, __LINE__);
}



} // namespace AlgorithmConcreteKernels
