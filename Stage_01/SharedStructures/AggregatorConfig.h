// AggregatorConfig.h
#pragma once

struct AggregatorConfig {
    bool expectsCamera   = true;
    bool expectsAlgorithm= true;
    bool expectsDisplay  = true;
    bool expectsPower    = true;   // disable if Lynsyn absent
    bool expectsSoC      = true;
};
/**
 * @brief AggregatorConfig is a structure that holds configuration settings for the aggregator.
 * 
 * This structure contains boolean flags that indicate whether certain components are expected
 * to be present in the system. These flags can be used to enable or disable specific features
 * or functionalities based on the presence of the corresponding components.
 */