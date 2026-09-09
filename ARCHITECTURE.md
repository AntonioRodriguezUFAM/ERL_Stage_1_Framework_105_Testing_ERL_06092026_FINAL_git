---

### 2. `ARCHITECTURE.md` (For Research Presentation)

This file details the internal logic, perfect for your presentation slides or thesis.

```markdown
# System Architecture & Logic

## 1. High-Level Data Flow
The framework follows a **Producer-Consumer** pattern using thread-safe `SharedQueue` and a dedicated `ThreadManager`.

```mermaid
graph LR
    CAM[Camera Thread] -->|ZeroCopy Ptr| ALG[Algorithm Thread]
    ALG -->|ZeroCopy Ptr| DISP[Display Thread]
    
    subgraph "Control Plane"
        MON[SoC & Power Monitor] --> AGG[Aggregator]
        CAM -.-> AGG
        ALG -.-> AGG
        
        AGG -->|Metrics| ERL[ERL Agent]
        ERL -->|Action| SCHED[Scheduler]
        SCHED -->|Controls| ALG
        SCHED -->|Sysfs| HW[Hardware]
    end