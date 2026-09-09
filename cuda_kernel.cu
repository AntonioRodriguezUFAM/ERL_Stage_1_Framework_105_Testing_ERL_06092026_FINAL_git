

// cuda_kernel.cu
#include <cstdio>
#include <cuda_runtime.h>

__global__ void helloWorldKernel() {
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    printf("Hello from GPU! Thread %d\n", idx);
}

extern "C" void runCudaHelloWorld() {
    printf("[CUDA Test] Launching kernel...\n");
    
    helloWorldKernel<<<2, 4>>>();
    
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        printf("[CUDA Error] %s\n", cudaGetErrorString(err));
    } else {
        printf("[CUDA Success] Kernel executed successfully!\n");
    }
}