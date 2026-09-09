//jetsonNanoInfo.h

#ifndef JETSON_NANO_INFO_H
#define JETSON_NANO_INFO_H
#include <chrono>

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


#endif // JETSON_NANO_INFO_H