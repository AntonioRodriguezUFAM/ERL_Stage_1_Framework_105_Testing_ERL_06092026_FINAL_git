# Jetson Nano Adaptive Heterogeneous Computing Framework

## ?? Project Overview
This project implements a high-performance, asynchronous C++ framework designed for **Evolutionary Reinforcement Learning (ERL)** on embedded heterogeneous systems (NVIDIA Jetson Nano).

The system continuously evaluates conflicting user constraints**Processing Latency**, **Power Consumption**, and **Thermal Balance**and optimizes workload distribution in real-time. It features a complete feedback loop: capturing data, monitoring hardware, evolving control strategies, and physically actuating system parameters (DVFS, affinity, concurrency).

---

## ?? Key Features

### 1. **Zero-Copy Pipeline**
* **V4L2 Capture**: Uses memory-mapped buffers managed by `std::shared_ptr` with custom deleters.
* **Zero Allocations**: Buffers are passed by reference from Camera -> Algorithm -> Display. No `malloc`/`memcpy` in the critical path.
* **Result**: Stable 30 FPS processing with <1% CPU overhead for data transport.

### 2. **Adaptive Heterogeneous Computing**
* **Dynamic Offloading**: The `AlgorithmModule` can switch execution between CPU (OpenCV) and GPU (CUDA) on the fly based on `RuntimeControls`.
* **Hardware Actuation**: The `Scheduler` directly manipulates `/sys/devices` to control:
    * CPU Frequencies (DVFS)
    * GPU Frequencies
    * Core Online/Offline status
    * Thread Affinity (packing vs. spreading)

### 3. **Evolutionary Reinforcement Learning (ERL)**
* **Population-Based Optimization**: Maintains a population of configuration "Genomes."
* **Fitness Evaluation**: Real-time scoring of system state against user constraints (Latency, Power, Temp).
* **Elitism**: Automatically selects and mutates the fittest strategies to adapt to changing workloads.

### 4. **Comprehensive Metrics Aggregation**
* **Unified Snapshot**: The `SystemMetricsAggregator` correlates asynchronous data streams:
    * **Camera**: FPS, Frame Size.
    * **Algorithm**: Inference Time, Jitter.
    * **Display**: End-to-End Latency.
    * **SoC**: CPU/GPU Load, Temperatures (via `tegrastats`).
    * **Power**: Precise Voltage/Current via **Lynsyn** hardware.

---

## ??? System Requirements
* **Hardware**: NVIDIA Jetson Nano (2GB/4GB) or compatible Tegra device.
* **Power Monitor**: Lynsyn Lite (optional, framework works without it).
* **OS**: Ubuntu 18.04 (JetPack 4.6).
* **Dependencies**:
    * `libopencv-dev`
    * `libsdl2-dev`
    * `libspdlog-dev`
    * `nlohmann-json3-dev`
    * `libusb-1.0-0-dev` (for Lynsyn)

---

## ? Quick Start

### 1. Build
```bash
mkdir build && cd build
cmake ..
make -j4


### 
