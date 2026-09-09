#pragma once

class IConstraints {
public:
    virtual ~IConstraints() = default;

    // Apply predefined constraints (e.g., power, thermal, timing) to the system
    virtual void applyConstraints() = 0;
};
