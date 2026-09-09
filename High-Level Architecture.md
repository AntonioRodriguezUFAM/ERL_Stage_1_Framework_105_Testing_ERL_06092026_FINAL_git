​High-Level Architecture:
​The provided code implements a complex, high-performance, real-time video processing and metrics-gathering pipeline. The architecture is modern, modular, and asynchronous, built around a "staged" pipeline design.

​Data Flow:
​Camera (DataConcrete) runs in its own thread, capturing YUYV frames from a V4L2 device.
​It uses a zero-copy mechanism (mmap'd buffers managed by std::shared_ptr custom deleters) to push frame data to two separate queues: cam2Alg (for processing) and cam2Disp (for original display).
​Algorithm (AlgorithmConcrete) runs in its own thread, consuming frames from the cam2Alg queue.
​It performs a selected image processing operation (either on CPU or GPU via CUDA) and places the newly created (processed) frame data into a third queue: alg2Disp.
​Display (SdlDisplayConcrete) runs on the main thread. It "drains" both the cam2Disp and alg2Disp queues (taking only the newest frame from each) to prevent display lag. It then renders the original and processed frames side-by-side.
​Metrics Flow (Parallel):
​SoC (SoCConcrete) and Power (LynsynMonitorConcrete) modules run in their own threads, independently polling for hardware statistics (CPU/GPU temps, power usage, etc.).
​All modules (Camera, Algorithm, Display, SoC, Power) push their individual statistics to a central Aggregator (SystemMetricsAggregatorConcrete_v3_2).
​The Aggregator, which runs its own flush thread, correlates these disparate stats using frameId and timestamps. It assembles them into complete SystemMetricsSnapshot objects, which are then written to both CSV and NDJSON files for logging and analysis.
​Orchestration:
​The SampleTestIntegrationCode_v13.cpp file contains the main() function and a ConfigManager class. This class is responsible for reading a config.json file, building the entire pipeline (instantiating all modules, queues, and the aggregator), and injecting all dependencies (like queues and the aggregator) into the modules that need them.
​A central ThreadManager is used to track all worker threads, allowing for a clean, graceful shutdown when the application receives a signal (like SIGINT).
