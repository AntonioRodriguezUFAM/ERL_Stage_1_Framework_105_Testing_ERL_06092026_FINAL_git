<!-- [MOD 09-22-2025] This README has been completely updated to reflect the final, working architecture -->

<!-- It now accurately describes the three-stage, zero-copy pipeline and provides a correct usage example. -->

-- TODAY
ERL Stage 1 Framework (Optimized Zero-Copy Version)
This repository contains the ERL Stage 1 Framework, a high-performance, modular system for asynchronous camera capture, real-time algorithmic processing, and visual display. The framework is designed with a clear separation of concerns, using a zero-copy, producer-consumer architecture to ensure high throughput and low latency, which is ideal for robotics, computer vision, and embedded AI applications.

Table of Contents
Overview

Architecture

Producer: The DataConcrete Module

Consumer 1: The AlgorithmConcrete Module

Consumer 2: The SdlDisplayConcrete Module

Orchestration: main and ThreadManager

Installation

Usage

Sample main-camera.cpp

Key Features

Directory Structure

Overview
The ERL Stage 1 Framework implements a complete, end-to-end video processing pipeline. It captures frames from a V4L2 camera, processes them in a separate algorithm thread, and displays both the original and processed video streams side-by-side in real-time.

The core of the architecture is a true zero-copy data flow mechanism. Instead of copying frame data between modules, V4L2 buffers are wrapped in std::shared_ptrs with a custom deleter. These pointers are passed through thread-safe queues, and the buffer is only returned to the camera driver when the last consumer (the algorithm or the display) is finished with it. This dramatically reduces CPU overhead and memory bandwidth, preventing common bottlenecks.

The primary modules are:

DataConcrete: The producer, responsible for camera interaction and frame capture.

AlgorithmConcrete: A consumer and producer, which processes frames.

SdlDisplayConcrete: The final consumer, responsible for rendering frames to the screen.

SystemMetricsAggregatorConcrete: A centralized service for collecting and logging performance metrics from all modules.

Architecture
The framework is a classic multi-threaded, producer-consumer pipeline.

Producer: The DataConcrete Module
Role: Interfaces directly with a V4L2 camera device.

Functionality:

Initializes and configures the camera (resolution, format, FPS, and number of buffers).

Runs a dedicated capture thread that waits for new frames from the driver using select().

When a frame arrives, it wraps the V4L2 buffer in a std::shared_ptr with a custom deleter.

Pushes a copy of this shared_ptr to two separate, thread-safe SharedQueues: one for the algorithm (cameraToAlgoQueue) and one for the display (cameraToDisplayQueue).

Implements a robust buffer state machine (AVAILABLE, QUEUED, IN_USE) to prevent race conditions and ensure buffers are never leaked.

Consumer 1: The AlgorithmConcrete Module
Role: A consumer of raw camera frames and a producer of processed frames.

Functionality:

Runs in its own thread, managed by the ThreadManager.

Pops raw frames from the cameraToAlgoQueue.

Performs an image processing operation (e.g., Invert, Grayscale, or a CUDA-based kernel).

Pushes a new shared_ptr containing the processed frame data to the algoToDisplayQueue.

Consumer 2: The SdlDisplayConcrete Module
Role: The final consumer in the pipeline, responsible for user-facing visualization.

Functionality:

Operates in the main application thread.

In its renderAndPollEvents() loop, it consumes frames from two input queues:

The cameraToDisplayQueue for the original, unprocessed video.

The algoToDisplayQueue for the processed video.

Uses a "drain latest" strategy, discarding stale frames in the queue to ensure the displayed image has the lowest possible latency.

Converts frame data from YUYV to RGB and renders the original and processed images side-by-side in an SDL window.

Orchestration: main and ThreadManager
main-camera.cpp: The application entry point. It is responsible for:

Instantiating all modules (Camera, Algorithm, Display, Aggregator).

Creating the SharedQueues and wiring the modules together.

Starting the modules in the correct order (consumers first, then the producer).

Running the main application loop, which is driven by the display.

Coordinating a graceful shutdown of all modules.

ThreadManager: A utility class that manages the lifecycle of the background threads for the camera and algorithm modules, ensuring they are properly started and joined.

Installation
Clone the repo:

git clone [https://github.com/your-repo/ERL_Stage_1_Framework.git](https://github.com/your-repo/ERL_Stage_1_Framework.git)

Install Dependencies: Ensure you have CMake, g++, spdlog, nlohmann-json, and SDL2 installed. For CUDA support, you will also need the NVIDIA CUDA Toolkit.

# Example for Ubuntu/Debian
sudo apt-get install build-essential cmake libspdlog-dev nlohmann-json3-dev libv4l-dev libsdl2-dev

Build:

mkdir build && cd build
cmake ..
make

Usage
The primary executable is camera_test. It can be run from the build directory:

./camera_test

The application will start the full pipeline, open an SDL window displaying the live video feed, and run for 10 seconds before shutting down and exporting performance metrics to the metrics_csv/ directory.

Sample main-camera.cpp
The main application orchestrates the entire pipeline:

#include "Stage_01/Concretes/DataConcrete_new.h"
#include "Stage_01/Concretes/AlgorithmConcrete_new.h"
#include "Stage_01/Concretes/SdlDisplayConcrete_new.h"
// ... other includes

int main() {
    // 1. Initialize ThreadManager, Queues, and Metrics Aggregator
    auto threadManager = std::make_shared<ThreadManager>();
    auto cameraToAlgoQueue = std::make_shared<SharedQueue<...>>(10);
    auto cameraToDisplayQueue = std::make_shared<SharedQueue<...>>(10);
    auto algoToDisplayQueue = std::make_shared<SharedQueue<...>>(10);
    auto metricsAggregator = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(...);

    // 2. Instantiate Modules
    CameraConfig camConfig(640, 480, 30, PixelFormat::YUYV, 8);
    auto camera = std::make_unique<DataConcrete>(camConfig, threadManager, cameraToAlgoQueue, cameraToDisplayQueue, metricsAggregator);

    auto algorithm = AlgorithmConcrete::createAlgorithmZeroCopy(AlgorithmType::Invert, cameraToAlgoQueue, algoToDisplayQueue, *threadManager, metricsAggregator);

    DisplayConfig dispConfig;
    auto display = std::make_unique<SdlDisplayConcrete>(cameraToDisplayQueue, algoToDisplayQueue, metricsAggregator);

    // 3. Start System (Consumers First)
    camera->openDevice("/dev/video0");
    camera->configure(camConfig);
    display->initializeDisplay(dispConfig.width, dispConfig.height);
    
    algorithm->startAlgorithm();
    camera->startCapture();

    // 4. Run Main Loop
    while (display->is_Running()) {
        display->renderAndPollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 5. Shut Down System
    display->closeDisplay();
    algorithm->stopAlgorithm();
    camera->stopCapture();
    metricsAggregator->stop();

    return 0;
}

Key Features
Asynchronous Pipeline: Camera, algorithm, and display modules run concurrently in separate threads, preventing stalls and maximizing throughput.

True Zero-Copy: V4L2 buffers are passed by reference using std::shared_ptr with a custom deleter, eliminating costly memory copies between modules.

Low-Latency Display: The display module uses a "drain latest" queueing strategy to ensure the rendered image is always the most up-to-date frame available.

Modular and Extensible: Each component is built against an interface, making it easy to swap in new implementations (e.g., a different algorithm or display technology) without changing the rest of the system.

Robust Resource Management: Smart pointers (std::unique_ptr, std::shared_ptr) are used throughout to ensure automatic memory and resource management.

Detailed Performance Logging: A centralized metrics aggregator collects and logs performance data (FPS, latency, etc.) from all modules to CSV for offline analysis.

Directory Structure
ERL_Stage_1_Framework/
├── CMakeLists.txt
├── main-camera.cpp
├── Stage_01/
│   ├── Interfaces/
│   │   ├── IData.h
│   │   ├── IAlgorithm.h
│   │   └── IDisplay.h
│   ├── SharedStructures/
│   │   ├── CameraConfig.h
│   │   ├── ZeroCopyFrameData.h
│   │   ├── SharedQueue.h
│   │   └── ThreadManager.h
│   └── Concretes/
│       ├── DataConcrete_new.h
│       ├── AlgorithmConcrete_new.h
│       ├── SdlDisplayConcrete_new.h
│       └── SdlDisplayConcrete_new.cpp
└── README.md
**--------------------------------------------------------------------------------------**
# ERL Stage 1 Framework
**--------------------------------------------------------------------------------------**
This repository contains the **ERL Stage 1 Framework**, a modular system for **asynchronous camera capture**, **algorithmic processing**, **SoC (System-on-Chip) performance monitoring**, and **profiling/logging**. The framework is designed with a **clear separation of concerns** and uses **interfaces** plus corresponding **concrete** implementations for data, algorithms, and SoC metrics.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
  - [Stage 1: SystemModelingFactory](#stage-1-systemmodelingfactory)
  - [Stage 2: SystemCaptureFactory](#stage-2-systemcapturefactory)
  - [Stage 3: SystemProfilingFactory](#stage-3-systemprofilingfactory)
  - [Stage 4: ConcreteComponentFactory](#stage-4-concretecomponentfactory)
  - [Stage 5: User Interface / main.cpp](#stage-5-user-interface--maincpp)
- [Installation](#installation)
- [Usage](#usage)
  - [Sample main.cpp](#sample-maincpp)
- [Features](#features)
- [Directory Structure](#directory-structure)
- [License](#license)

---

## Overview

The ERL Stage 1 Framework revolves around capturing frames from **one or more cameras**, optionally **processing** them in an **algorithm** thread, **monitoring** SoC performance, and periodically **profiling** and **logging** system metrics to CSV. The design uses the following primary interfaces:

- **IData / DataConcrete**: Interacts with camera data (V4L2 or another backend).
- **IAlgorithm / AlgorithmConcrete**: Encapsulates an algorithm (e.g., a vision or ML process).
- **ISoC / SoCConcrete**: Monitors SoC (e.g., Jetson Nano) via tegrastats or similar.
- **ISystemProfiling**: Allows a consumer (like `SystemProfilingFactory`) to retrieve metrics (SoC stats, camera FPS, queue sizes, and algorithm performance).

These modules are **wired** together in an **asynchronous** pipeline, ensuring each runs **independently** in its own thread without blocking others. A **SharedQueue** decouples camera producers from the algorithm consumer, and `ISystemProfiling` decouples metric providers from the profiling consumer.

---

## Architecture

### **Stage 1: SystemModelingFactory**
- **Creates and configures** the building blocks:
  - **IData** objects (e.g., `DataConcrete`) for cameras.
  - **IAlgorithm** objects (e.g., `AlgorithmConcrete`) for frame processing.
  - **ISoC** objects (e.g., `SoCConcrete`) for SoC performance monitoring.
  - Optional modules (IRawDataFormat, IConstraints, IOptimization, ILearning, etc.).
- Typically used if you want a single factory to produce all “concrete” components with some default configs or branching logic.

### **Stage 2: SystemCaptureFactory**
- **Coordinates the asynchronous capture system**:
  - **Multiple cameras** each run a `captureLoop()` in separate threads, producing frames into a **SharedQueue**.
  - An **algorithm thread** pulls frames from the queue via `algorithmLoop()`.
  - An optional **display thread** (`displayLoop()`) can render frames or UI.
  - A **SoC monitoring thread** (`monitorLoop()`) periodically queries SoC metrics.
- **Implements `ISystemProfiling`** to publish metrics:
  - `getSoCMetrics()`: e.g., CPU usage, temp.
  - `getCameraMetrics(int index)`: e.g., FPS and queue size.
  - `getAlgorithmMetrics()`: e.g., algorithm FPS and processing time.
- Asynchronous design **decouples** cameras from the algorithm, avoiding blocking calls.

### **Stage 3: SystemProfilingFactory**
- **Periodically queries** `ISystemProfiling` interface (provided by `SystemCaptureFactory`).
- **Writes** these metrics to a CSV file in a background **profilingThread** at a set interval (e.g., once per second).
- Minimizes overhead by simply calling the metrics functions that retrieve counters or stored stats.

### **Stage 4: ConcreteComponentFactory**
- A **high-level** or “mini main” factory that:
  - Creates `SystemCaptureFactory` with specific SoC, Algorithm, and camera instances.
  - Creates `SystemProfilingFactory` referencing that `SystemCaptureFactory`.
  - Offers convenient methods (`startSystem()`, `stopSystem()`) to **initialize** the entire pipeline or **shut it down**.

### **Stage 5: User Interface / main.cpp**
- A final **application entry point** that:
  - **Instantiates** the `ConcreteComponentFactory` (or directly uses `SystemCaptureFactory` and `SystemProfilingFactory`).
  - Applies custom camera, algorithm, and SoC configurations.
  - Starts the system, runs for a desired time, demonstrates `pauseAll()`/`resumeAll()`, etc.
  - Stops profiling and capture and then exits.
---
## Installation

1. **Clone the repo**:
   ```bash
   git clone https://github.com/yourusername/ERL_Stage_1_Framework_03.git
   ```
2. **Navigate to the project directory**:
   ```bash
   cd ERL_Stage_1_Framework_03
   ```
3. **Install dependencies** (e.g., CMake, spdlog, SDL2, etc.):
   ```bash
   # Example
   sudo apt-get install libspdlog-dev libsdl2-dev
   # or your local environment's method
   ```
4. **Build**:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   # Optionally, make install
   ```
---

## Usage

1. **Create** or **configure** cameras, SoC, and algorithm with `SystemCaptureFactory` or via `SystemModelingFactory`.
2. **Optionally** create a `SystemProfilingFactory` referencing the `SystemCaptureFactory` as `ISystemProfiling`.
3. **Initialize** capture with `captureFactory.initializeCapture(...)` and **start profiling** with `profilingFactory.startProfiling("metrics.csv")`.
4. **Run** your application, optionally `pauseAll()` or `resumeAll()`.
5. **Stop** profiling and capture with `profilingFactory.stopProfiling()` and `captureFactory.stopCapture()`.

### Sample main.cpp

```cpp
int main() {
    try {
        // Create SoC, Algorithm, Cameras
        auto soc     = std::make_shared<SoCConcrete>();
        auto algo    = std::make_shared<AlgorithmConcrete>();
        auto camera0 = std::make_shared<DataConcrete>();
        auto camera1 = std::make_shared<DataConcrete>();

        // Build SystemCaptureFactory
        SystemCaptureFactory captureFactory(soc, algo, camera0, camera1);

        // Build SystemProfilingFactory from captureFactory (ISystemProfiling)
        SystemProfilingFactory profilingFactory(captureFactory);

        // Camera & Algorithm configs
        CameraConfig camConfig{640, 480, 30, "YUYV"};
        AlgorithmConfig algoConfig{4};

        // Initialize & start
        captureFactory.initializeCapture(camConfig, algoConfig);
        profilingFactory.startProfiling("PerformanceMetrics.csv");

        // Simulate runtime
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // Pause, wait, resume
        captureFactory.pauseAll();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        captureFactory.resumeAll();

        // Stop
        profilingFactory.stopProfiling();
        captureFactory.stopCapture();
    } 
    catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }
    return 0;
}
```

---

## Features

- **Asynchronous Pipeline**: Multiple cameras, SoC monitoring, algorithm threads, and optional display run concurrently.  
- **Modular Design**: Each stage (Modeling, Capture, Profiling) is separated by interfaces, enabling easy swapping or testing.  
- **Performance Logging**: Collect SoC stats, camera FPS, and algorithm processing times to CSV for offline analysis.  
- **Error Handling & Callbacks**: Each component can report errors to a higher-level callback.  
- **Extensible**: Add more cameras, additional algorithms, or more SoC stats by expanding the relevant interfaces/implementations.

---

## Directory Structure

A typical layout might look like this:

```
YourProject/
├── CMakeLists.txt
├── main.cpp
├── Stage_01/
│   ├── Interfaces/
│   │   ├── IData.h
│   │   ├── IAlgorithm.h
│   │   ├── ISoC.h
│   │   ├── ISystemProfiling.h
│   ├── SharedStructures/
│   │   ├── FrameData.h
│   │   ├── CameraConfig.h
│   │   ├── AlgorithmConfig.h
│   │   ├── SoCConfig.h
│   │   ├── SharedQueue.h
│   ├── Concretes/
│   │   ├── DataConcrete.h
│   │   ├── AlgorithmConcrete.h
│   │   ├── SoCConcrete.h
│   │   └── SdlDisplayConcrete.h
│   ├── Factories/
│   │   ├── SystemModelingFactory.h
│   │   ├── SystemCaptureFactory.h
│   │   └── SystemCaptureFactory.cpp
├── Stage_02/
│   ├── Factories/
│   │   └── systemProfilingFactory.h
├── ...
└── README.md
```

---

## License

This project is licensed under the [**Your License**] License. See the [LICENSE](LICENSE) file for details.

---

### Conclusion

The **ERL Stage 1 Framework** offers a **full-stack** asynchronous solution for camera capture, real-time algorithmic processing, SoC performance monitoring, and CSV-based profiling. By following the **five-stage** architecture—**SystemModelingFactory**, **SystemCaptureFactory**, **SystemProfilingFactory**, **ConcreteComponentFactory**, and a user-level **main.cpp**—you gain:

- **Modular** design.  
- **Scalable** concurrency.  
- **Easy** performance logging (CSV).  
- **Clear** testability and extension points.

Use this framework as a **foundation** for advanced robotics, computer vision, or embedded AI applications where you need to capture camera data in real time, run algorithms, and measure system performance metrics with minimal overhead.
