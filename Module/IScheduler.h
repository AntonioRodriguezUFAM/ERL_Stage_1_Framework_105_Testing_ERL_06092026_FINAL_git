// IScheduler.h
//
// Pure-virtual scheduling interface used by GeneticAlgorithm and
// EvolutionarySelector.  Depending on this interface instead of the concrete
// Scheduler class breaks the compile-time dependency on the 2 000-line
// Scheduler.h header, making the GA unit-testable without pulling in all
// sysfs-writing machinery.

#pragma once

#include "RuntimeControls.h"  // RLAction, MetricsSnapshot, RuntimeControls
#include "MetricsSnapshot.h"  // hrl::MetricsSnapshot definition

namespace hrl {

class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void reset() = 0; // Reset to defult

    // Apply a scheduling decision to the runtime controls.
    virtual void apply(const RLAction& action,
                       const MetricsSnapshot& metrics,
                       RuntimeControls& rt) = 0;
};

} // namespace hrl
