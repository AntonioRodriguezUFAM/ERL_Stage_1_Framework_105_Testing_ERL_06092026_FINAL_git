#pragma once

class IOptimization {
public:
    virtual ~IOptimization() = default;

    // Optimize resource usage (e.g., CPU/GPU load balancing, memory management)
    virtual void optimizeResources() = 0;
};
