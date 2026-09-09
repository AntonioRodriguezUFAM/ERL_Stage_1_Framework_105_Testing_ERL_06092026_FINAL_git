#include <chrono>

// sharedStructures.h

/***********************************************
 * Shared Structures
 ***********************************************/

 /**
 * @brief Enum that wraps V4L2 pixel format codes
 */
enum class PixelFormat : uint32_t {
    YUYV = V4L2_PIX_FMT_YUYV, 
    MJPG = V4L2_PIX_FMT_MJPEG,
    // Add more as needed...
};


 struct CameraConfig {
    int width ;//=320;     //640;               ///< Desired capture width (pixels)
    int height ;//=240;    // 480;             ///< Desired capture height (pixels)
    int fps ;//= 30;                 ///< Desired frames per second
    //std::string pixelFormat = "YUYV";  ///< (e.g., "YUYV", "MJPEG", "RGB24").
    //uint32_t pixelFormat;  // Now stores V4L2 fourcc codes directly
    PixelFormat pixelFormat;

    // Optionally add a constructor to make usage simpler:
  // Optional constructor
    CameraConfig(int w = 640, int h = 480, int f = 30, PixelFormat pf = PixelFormat::YUYV)
        : width(w), height(h), fps(f), pixelFormat(pf)
    {}
};

// CameraConfig.h

// DisplayConfig.h

/***********************************************
 * Shared Structures
 ***********************************************/

 /**
 * @brief Data structure for storing Jetson Nano performance metrics.
 */
struct JetsonNanoInfo {
    int RAM_In_Use_MB, Total_RAM_MB;
    int LFB_Size, Block_Max_MB;
    int SWAP_In_Use_MB, Total_SWAP_MB;
    int Cached_MB;
    int used_IRAM_kB, total_IRAM_kB, lfb_kB;
    int CPU1_Utilization_Percent, CPU1_Frequency_MHz;
    int CPU2_Utilization_Percent, CPU2_Frequency_MHz;
    int CPU3_Utilization_Percent, CPU3_Frequency_MHz;
    int CPU4_Utilization_Percent, CPU4_Frequency_MHz;
    int EMC_Frequency_Percent, GR3D_Frequency_Percent;
    float PLL_Temperature_C, CPU_Temperature_C;
    float PMIC_Temperature_C, GPU_Temperature_C;
    float AO_Temperature_C, Thermal_Temperature_C;
    std::chrono::system_clock::time_point timestamp; // Add a timestamp

    JetsonNanoInfo() // Constructor initializes all fields to 0
        : RAM_In_Use_MB(0), Total_RAM_MB(0),
        LFB_Size(0), Block_Max_MB(0),
        SWAP_In_Use_MB(0), Total_SWAP_MB(0), Cached_MB(0),
        used_IRAM_kB(0), total_IRAM_kB(0), lfb_kB(0),
        CPU1_Utilization_Percent(0), CPU1_Frequency_MHz(0),
        CPU2_Utilization_Percent(0), CPU2_Frequency_MHz(0),
        CPU3_Utilization_Percent(0), CPU3_Frequency_MHz(0),
        CPU4_Utilization_Percent(0), CPU4_Frequency_MHz(0),
        EMC_Frequency_Percent(0), GR3D_Frequency_Percent(0),
        PLL_Temperature_C(0.0f), CPU_Temperature_C(0.0f),
        PMIC_Temperature_C(0.0f), GPU_Temperature_C(0.0f),
        AO_Temperature_C(0.0f), Thermal_Temperature_C(0.0f),
        timestamp(std::chrono::system_clock::now()) {} // Initialize timestamp
};



struct CameraStats {
    std::chrono::system_clock::time_point timestamp;
    double fps{0.0};
    int frameNumber{0};
};

struct AlgorithmStats {
    std::chrono::system_clock::time_point timestamp;
    double inferenceTimeMs{0.0};
    double confidenceScore{0.0}; // example
};

struct DisplayStats {
    std::chrono::system_clock::time_point timestamp;
    double renderTimeMs{0.0};
    int droppedFrames{0};
};


struct PowerStats {
    std::chrono::system_clock::time_point timestamp;
    double voltage{0.0};
    double current{0.0};
    double power() const { return voltage * current; }
};