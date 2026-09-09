
// IModule.h

// Introduce a Core Module Base Interface
/**
 * @brief 
 * This interface defines the core functionalities that any module should implement.
 * Each concrete module (DataConcrete, AlgorithmConcrete, SdlDisplayConcrete, SoCConcrete, LynsynMonitorConcrete, SystemMetricsAggregator) 
 * should inherit and encapsulate their own config + validation.
 * 
 */

#ifndef IMODULE_H
#define IMODULE_H

class IModule {
public:
    virtual bool validate() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual ~IModule() = default;
};
#endif // IMODULE_H 