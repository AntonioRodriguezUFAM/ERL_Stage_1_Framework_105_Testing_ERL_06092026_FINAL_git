#pragma once

class ILearning {
public:
    virtual ~ILearning() = default;

    // Learn patterns or models from data (e.g., machine learning inference or training)
    virtual void learnPattern() = 0;

    // Adapt the system's behavior based on learned insights (e.g., dynamic optimization)
    virtual void adapt() = 0;
};
