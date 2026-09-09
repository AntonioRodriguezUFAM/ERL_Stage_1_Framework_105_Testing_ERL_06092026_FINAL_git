
/*
Step 1: Create a Simple Test Main (LynsynMain.cpp)
To test, you need a entry point. Create main.cpp in the same directory as LynsynDevice.h and LynsynDevice.cpp:

*/
#include "LynsynDevice.h"
#include <iostream>
#include <chrono>
#include <thread>

int main() {
    try {
        LynsynDevice device(20, 10);  // maxTries=20, maxFails=10

        std::cout << "Device initialized. HW Version: " << static_cast<int>(device.getHwVer()) << std::endl;

        // Arm periodic sampling (adjust params as needed)
        device.startPeriodSampling(10.0, 0x1);  // 10 sec, core 0

        // Poll samples for testing (run for ~10 sec)
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
            LynsynSample sample;
            if (device.getNextSample(sample)) {
                std::cout << "Sample time: " << lynsyn_cyclesToSeconds(sample.time) << "s" << std::endl;
                for (unsigned i = 0; i < device.getNumSensors(); ++i) {
                    std::cout << "Sensor " << i << ": Current=" << sample.current[i] << "A, Voltage=" << sample.voltage[i] << "V" << std::endl;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Backoff if no sample
            }
        }

        std::cout << "Test complete." << std::endl;
    } catch (const LynsynException& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}