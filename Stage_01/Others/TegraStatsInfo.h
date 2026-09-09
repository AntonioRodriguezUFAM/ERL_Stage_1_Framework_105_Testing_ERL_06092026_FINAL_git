//TegraStatsInfo.h
#pragma once
#include <memory>
#include <stdexcept>
#include <cstdio>
#include <array>
#include <string>
#include <iostream>

// TegraStatsInfo handles running tegrastats and reading its output line by line.
class TegraStatsInfo {
private:
    std::unique_ptr<FILE, decltype(&pclose)> pipe;
    std::string command;

public:
    // Initialize tegrastats with the given command. If the command cannot be run,
    // an exception is thrown. The caller should handle this exception.
    
    //TegraStatsInfo(const std::string& cmd) : command(cmd) {}

    /**
 * @brief TegraStatsInfo handles running `tegrastats` and reading its output line by line.
 */
    TegraStatsInfo(const std::string& cmd)
        : pipe(popen(cmd.c_str(), "r"), pclose)
    {
        if (!pipe) {
            throw std::runtime_error("popen() failed: Could not run command: " + cmd);
        }
    }

    // Read the next line of tegrastats output.
    // If a line is successfully read, return it as a std::string.
    // If no line is available (e.g., EOF or tegrastats stopped), return an empty string.
    //
    // The caller should consider what to do if the result is empty:
    // - If it remains empty for multiple reads, tegrastats may have stopped or crashed.
    // - The caller could log a warning, attempt to restart tegrastats, or handle it gracefully.

        /**
     * @brief Read the next line of tegrastats output. Returns an empty string if none.
     */

    std::string getInformation() {
        std::array<char, 512> buffer;
        // string: "RAM 1413/1952MB (lfb 2x2MB) SWAP 1161/13264MB (cached 90MB) CPU [0%@1479,0%@1479,100%@1479,100%@1479] EMC_FREQ 0% GR3D_FREQ 0% PLL@54.5C CPU@57C PMIC@50C GPU@56.5C AO@62C thermal@56C"
        //Counting each character, we get the length of the string : 182 characters. Therefore, the minimum buffer size needed to store this string is 182 characters.
        if (fgets(buffer.data(), (int)buffer.size(), pipe.get()) != nullptr) {
            return std::string(buffer.data());
        }

        // If we get here, no more output is available or tegrastats terminated.
        return std::string();
    }

    // Note: This class is not inherently thread-safe. If multiple threads need to call
    // getInformation(), consider adding external synchronization or confining each
    // TegraStatsInfo instance to a single thread.
    //
    // Additional Notes:
    // - If you need to ensure tegrastats stops when stopSoc() is called,
    //   you could modify TegraStatsInfo to close the pipe or send a kill signal to the tegrastats process.
    // - If you need more logging, you can add additional print statements or integrate a logging framework.
    // - If you require non-blocking behavior or timeouts, additional logic beyond fgets() will be necessary.
};

