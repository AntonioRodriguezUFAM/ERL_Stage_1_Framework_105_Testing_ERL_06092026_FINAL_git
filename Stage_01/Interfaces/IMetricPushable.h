// IMetricPushable.h


// IMetricPushable.h
#pragma once
#include <chrono>

class IMetricPushable {
public:
    virtual ~IMetricPushable() = default;
    virtual void pushMetrics(const std::chrono::system_clock::time_point& ts) = 0;
};
