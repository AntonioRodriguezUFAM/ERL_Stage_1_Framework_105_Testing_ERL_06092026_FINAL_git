// SystemMetricsSnapshot.h


#include <chrono>
struct SystemMetricsSnapshot {
    std::chrono::system_clock::time_point timestamp;

    // From SoCConcrete
    JetsonNanoInfo socInfo;

    // From DataConcrete
    CameraStats cameraStats;

    // From AlgorithmConcrete
    AlgorithmStats algorithmStats;

    // From SdlDisplayConcrete
    DisplayStats displayStats;

    // From LynsynMonitorConcrete
    PowerStats powerStats;
};
