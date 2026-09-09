#pragma once

class IRawDataFormat {
public:
    virtual ~IRawDataFormat() = default;

    // Initialize necessary components for raw data handling
    virtual void initializeComponents() = 0;

    // Format raw data into a usable structure
    virtual void formatData() = 0;
};
