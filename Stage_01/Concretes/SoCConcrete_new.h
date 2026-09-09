
/// ==================================25-05-2025=====================================

#pragma once

//#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "../Interfaces/ISoC.h"
#include "../SharedStructures/SoCConfig.h"
#include "../Interfaces/ISystemMetricsAggregator.h"
#include "../SharedStructures/allModulesStatcs.h"

#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <regex>
#include <array>    // [PROD] Explicit std::array header
#include <fstream>
#include <iomanip>
#include <random> 

class SoCConcrete : public ISoC {
public:
    SoCConcrete(std::shared_ptr<ISystemMetricsAggregator> aggregator);
    ~SoCConcrete() override;

    bool initializeSoC() override;
    std::string getSpecs() override;
    JetsonNanoInfo getPerformance() const override;
    void stopSoC() override;

    bool configure(const SoCConfig& config) override;
    void setErrorCallback(std::function<void(const std::string&)> cb) override;
    void pushSoCStats(const JetsonNanoInfo& stats) override;
    void pauseSoC();
    void resumeSoC();
    bool isMonitoringActive() const; 

private:
    void monitorPerformance();
    std::string getSoCInformationLine();
    JetsonNanoInfo parseTegraStats(const std::string& output);
    JetsonNanoInfo parseTx2Stats(const std::string& output);
    JetsonNanoInfo parseXavierStats(const std::string& output);
    JetsonNanoInfo parseRpiStats(const std::string& output);
    void logPartialParse(const std::string& msg);
    void exportToCSV(const JetsonNanoInfo& info, const std::string& filePath);
    void reportError(const std::string& msg);
    bool verifyTegraStatsOutput(); 

    std::unique_ptr<FILE, decltype(&pclose)> pipe_{nullptr, pclose};
    std::string command_;
    SoCConfig config_;
    std::thread monitoringThread_;
    std::atomic<bool> monitoringActive_{false};
    std::atomic<bool> monitoringPaused_{false};
    mutable std::mutex performanceMutex_;
    JetsonNanoInfo lastPerformanceData_;
    std::function<void(const std::string&)> errorCallback_;
    std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;
};

inline SoCConcrete::SoCConcrete(std::shared_ptr<ISystemMetricsAggregator> aggregator)
    : pipe_(nullptr, pclose), metricAggregator_(std::move(aggregator)) {}

inline SoCConcrete::~SoCConcrete() {
    stopSoC();
}

inline bool SoCConcrete::configure(const SoCConfig& config) {
    config_ = config;
    if (config_.customCommand.empty()) {
        config_.customCommand = "tegrastats --interval " + std::to_string(config_.pollIntervalMs);
    }
    
    // [PROD] Keep tegrastats' emission cadence coupled to pollIntervalMs:
    // This loop consumes ONE line per poll, so a faster producer grows the
    // pipe buffer without bound and pushed samples lag wall time by many
    // seconds while carrying fresh parse-time timestamps - silently
    // misaligning SoC/temperature data inside the aggregator's windows.
    if (config_.customCommand.find("tegrastats") != std::string::npos &&
        config_.customCommand.find("--interval") == std::string::npos) {
        config_.customCommand += " --interval " + std::to_string(config_.pollIntervalMs);
    }

    spdlog::info("[SoCConcrete] Configured SoC with command='{}', pollIntervalMs={}, exportCSV={}, boardType={}",
                 config_.customCommand, config_.pollIntervalMs, config_.exportCSV,
                 static_cast<int>(config_.boardType));
    return true;
}

inline bool SoCConcrete::initializeSoC() {
    if (monitoringActive_) {
        spdlog::warn("[SoCConcrete] SoC is already initialized and running.");
        return true;
    }

    try {
        command_ = config_.customCommand;
        pipe_ = std::unique_ptr<FILE, decltype(&pclose)>(
            popen(command_.c_str(), "r"),
            pclose
        );
        if (!pipe_) {
            throw std::runtime_error("Could not popen() command: " + command_);
        }

        if (!verifyTegraStatsOutput()) {
            pipe_.reset();
            throw std::runtime_error("tegrastats command failed to produce valid output");
        }

        monitoringActive_ = true;
        monitoringPaused_ = false;
        monitoringThread_ = std::thread(&SoCConcrete::monitorPerformance, this);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!monitoringActive_) {
            pipe_.reset();
            monitoringThread_.join();
            throw std::runtime_error("Monitoring thread failed to start");
        }

        spdlog::info("[SoCConcrete] SoC monitoring started with command '{}'.", command_);
        return true;
    }
    catch (const std::exception& ex) {
        monitoringActive_ = false;
        pipe_.reset();
        if (monitoringThread_.joinable()) {
            monitoringThread_.join();
        }
        reportError("[SoCConcrete] Initialization failed: " + std::string(ex.what()));
        return false;
    }
}

inline bool SoCConcrete::verifyTegraStatsOutput() {
    std::string line = getSoCInformationLine();
    if (line.empty()) {
        reportError("[SoCConcrete] No data from tegrastats command");
        return false;
    }

    static const std::regex ram_pattern(R"(RAM\s+\d+/\d+MB)");
    std::smatch matches;
    if (!std::regex_search(line, matches, ram_pattern)) {
        reportError("[SoCConcrete] tegrastats output invalid: missing RAM pattern");
        return false;
    }

    return true;
}

inline void SoCConcrete::stopSoC() {
    spdlog::info("[SoCConcrete] Stopping SoC monitoring...");

    monitoringActive_ = false;
    monitoringPaused_ = false;

    if (monitoringThread_.joinable()) {
        monitoringThread_.join();
    }

    if (pipe_) {
        spdlog::info("[SoCConcrete] Closing tegrastats pipe.");
        int closeRet = pclose(pipe_.release());
        if (closeRet != 0) {
            spdlog::error("[SoCConcrete] pclose failed with code {}", closeRet);
        }
    }

    spdlog::info("[SoCConcrete] Stopped.");
}

inline void SoCConcrete::pauseSoC() {
    if (monitoringActive_) {
        monitoringPaused_ = true;
        spdlog::info("[SoCConcrete] SoC monitoring is now paused.");
    }
}

inline void SoCConcrete::resumeSoC() {
    if (monitoringActive_) {
        monitoringPaused_ = false;
        spdlog::info("[SoCConcrete] SoC monitoring is now resumed.");
    }
}

inline std::string SoCConcrete::getSpecs() {
    switch (config_.boardType) {
        case BoardType::JetsonNano:
            return "Jetson Nano: Quad-core ARM Cortex-A57 @ 1.43 GHz, 128-core Maxwell GPU, 4GB LPDDR4";
        case BoardType::JetsonTX2:
            return "Jetson TX2: NVIDIA Denver2 + ARM A57 CPU, 256-core Pascal GPU, 8GB LPDDR4";
        case BoardType::JetsonXavier:
            return "Jetson Xavier: 8-core ARM v8.2 64-bit, 512-core Volta GPU, 16GB LPDDR4";
        case BoardType::RaspberryPi:
            return "Raspberry Pi: e.g., 4-core ARM Cortex-A72, VideoCore VI GPU, up to 4GB LPDDR4";
        default:
            return "Unknown board type: limited specs available.";
    }
}

inline JetsonNanoInfo SoCConcrete::getPerformance() const {
    std::lock_guard<std::mutex> lock(performanceMutex_);
    return lastPerformanceData_;
}

inline void SoCConcrete::pushSoCStats(const JetsonNanoInfo& stats) {
    // [F26-FIX] Direct forward to aggregator ensures thermal and energy metrics reach system snapshot.
    // [F26-FIX+] Scoped mutex lock restores local cache so getPerformance() remains functional.
    {
        std::lock_guard<std::mutex> lock(performanceMutex_);
        lastPerformanceData_ = stats;
    }

    if (metricAggregator_) {
        metricAggregator_->pushSoCStats(stats);
    }
}

inline void SoCConcrete::setErrorCallback(std::function<void(const std::string&)> cb) {
    errorCallback_ = std::move(cb);
}

inline bool SoCConcrete::isMonitoringActive() const {
    return monitoringActive_.load();
}

inline void SoCConcrete::monitorPerformance() {
    spdlog::debug("[SoCConcrete] monitorPerformance() thread started.");

    while (monitoringActive_) {
        if (monitoringPaused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::string line = getSoCInformationLine();
        if (!line.empty()) {
            JetsonNanoInfo info;
            switch (config_.boardType) {
                case BoardType::JetsonNano:
                    info = parseTegraStats(line);
                    break;
                case BoardType::JetsonTX2:
                    info = parseTx2Stats(line);
                    break;
                case BoardType::JetsonXavier:
                    info = parseXavierStats(line);
                    break;
                case BoardType::RaspberryPi:
                    info = parseRpiStats(line);
                    break;
                default:
                    info = parseTegraStats(line);
                    break;
            }
            pushSoCStats(info);
            if (config_.exportCSV) {
                exportToCSV(info, config_.csvPath);
            }
        } else {
            reportError("[SoCConcrete] No data from SoC command. Retrying...");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config_.pollIntervalMs));
    }
    pipe_.reset();
    spdlog::debug("[SoCConcrete] monitorPerformance() thread exiting.");
}

inline std::string SoCConcrete::getSoCInformationLine() {
    if (!pipe_) {
        return {};
    }
    std::array<char, 512> buffer;
    if (fgets(buffer.data(), buffer.size(), pipe_.get()) != nullptr) {
        return std::string(buffer.data());
    }
    return {};
}

inline JetsonNanoInfo SoCConcrete::parseTegraStats(const std::string& output) {
    JetsonNanoInfo info;
    info.timestamp = std::chrono::system_clock::now();

    static const std::regex ram_pattern(R"(RAM\s+(\d+)/(\d+)MB)");
    static const std::regex lfb_pattern(R"(lfb\s+(\d+)x(\d+)MB)");
    static const std::regex swap_pattern(R"(SWAP\s+(\d+)/(\d+)MB)");
    static const std::regex cached_pattern(R"(cached\s+(\d+)MB)");
    static const std::regex iram_pattern(R"(IRAM\s+(\d+)/(\d+)kB\s*\(lfb\s+(\d+)kB\))");
    static const std::regex cpu_pattern(
        R"(CPU\s+\[(?:(\d+)%@(\d+)|off),(?:(\d+)%@(\d+)|off),(?:(\d+)%@(\d+)|off),(?:(\d+)%@(\d+)|off)\])");
    // [TELEMETRY-FIX3] Parse EMC and GR3D independently.
    // Jetson Nano tegrastats variants may emit either:
    //   EMC_FREQ 12% GR3D_FREQ 37%
    // or:
    //   EMC_FREQ 12%@1600 GR3D_FREQ 37%@921
    // We only need the utilization percentages here, so do not require @frequency.
    static const std::regex emc_freq_pattern(R"(EMC_FREQ\s+(\d+)%)");
    static const std::regex gr3d_freq_pattern(R"(GR3D_FREQ\s+(\d+)%)");
    static const std::regex temp_pattern(R"(PLL@([\d.]+)C\s+CPU@([\d.]+)C\s+PMIC@([\d.]+)C\s+GPU@([\d.]+)C\s+AO@([\d.]+)C\s+thermal@([\d.]+)C)");

    std::smatch matches;
    bool foundSomething = false;

    if (std::regex_search(output, matches, ram_pattern) && matches.size() >= 3) {
        info.RAM_In_Use_MB = std::stoi(matches[1]);
        info.Total_RAM_MB = std::stoi(matches[2]);
        foundSomething = true;
    } else if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse RAM from line: " + output);
    }

    if (std::regex_search(output, matches, lfb_pattern) && matches.size() >= 3) {
        info.LFB_Size_MB = std::stoi(matches[1]);
        info.Block_Max_MB = std::stoi(matches[2]);
    } else if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse LFB from line: " + output);
    }

    if (std::regex_search(output, matches, swap_pattern) && matches.size() >= 3) {
        info.SWAP_In_Use_MB = std::stoi(matches[1]);
        info.Total_SWAP_MB = std::stoi(matches[2]);
    } else if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse SWAP from line: " + output);
    }

    if (std::regex_search(output, matches, cached_pattern) && matches.size() >= 2) {
        info.Cached_MB = std::stoi(matches[1]);
    } else if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse CACHE from line: " + output);
    }

    if (std::regex_search(output, matches, iram_pattern) && matches.size() >= 4) {
        info.used_IRAM_kB = std::stoi(matches[1]);
        info.total_IRAM_kB = std::stoi(matches[2]);
        info.lfb_kB = std::stoi(matches[3]);
    } else if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse IRAM from line: " + output);
    }

    if (std::regex_search(output, matches, cpu_pattern) && matches.size() >= 9) {
        auto parseCpu = [](const std::string& utilStr, const std::string& freqStr, double& util, double& freq) {
            if (utilStr.empty() || freqStr.empty()) {
                util = 0;
                freq = 0;
            } else {
                util = std::stoi(utilStr);
                freq = std::stoi(freqStr);
            }
        };
        parseCpu(matches[1], matches[2], info.CPU1_Utilization_Percent, info.CPU1_Frequency_MHz);
        parseCpu(matches[3], matches[4], info.CPU2_Utilization_Percent, info.CPU2_Frequency_MHz);
        parseCpu(matches[5], matches[6], info.CPU3_Utilization_Percent, info.CPU3_Frequency_MHz);
        parseCpu(matches[7], matches[8], info.CPU4_Utilization_Percent, info.CPU4_Frequency_MHz);
        foundSomething = true;
    } else if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse CPU usage from line: " + output);
    }

    // [TELEMETRY-FIX3] Parse EMC and GR3D independently so a format change in
    // one token cannot zero the other.  Keep the 500 ms sampling cadence unchanged.
    bool emcParsed = false;
    bool gr3dParsed = false;

    if (std::regex_search(output, matches, emc_freq_pattern) && matches.size() >= 2) {
        info.EMC_Frequency_Percent = std::stoi(matches[1]);
        emcParsed = true;
        foundSomething = true;
    }

    if (std::regex_search(output, matches, gr3d_freq_pattern) && matches.size() >= 2) {
        info.GR3D_Frequency_Percent = std::stoi(matches[1]);
        gr3dParsed = true;
        foundSomething = true;
    }

    // Rate-limited production diagnostics: enough to distinguish parser failure
    // from genuine 0% tegrastats samples without flooding a 2 Hz benchmark log.
    static std::atomic<uint64_t> gr3dSamples{0};
    static std::atomic<uint64_t> gr3dParseFails{0};
    static std::atomic<uint64_t> gr3dNonZeroSamples{0};

    const uint64_t sampleNo = ++gr3dSamples;
    if (gr3dParsed && info.GR3D_Frequency_Percent > 0.0) {
        ++gr3dNonZeroSamples;
    }
    if (!gr3dParsed) {
        const uint64_t failNo = ++gr3dParseFails;
        if (failNo == 1 || failNo % 60 == 0) {
            spdlog::warn(
                "[SoCConcrete][GR3D] parse FAILED (fail={} sample={}) | raw='{}'",
                failNo, sampleNo, output);
        }
    } else if (sampleNo <= 3 || sampleNo % 60 == 0) {
        spdlog::info(
            "[SoCConcrete][GR3D] sample={} parsed={}%, nonzero_samples={}, parse_failures={}",
            sampleNo,
            info.GR3D_Frequency_Percent,
            gr3dNonZeroSamples.load(),
            gr3dParseFails.load());
    }

    if (!emcParsed && config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse EMC_FREQ from line: " + output);
    }
    if (!gr3dParsed && config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] Could not parse GR3D_FREQ from line: " + output);
    }

    if (std::regex_search(output, matches, temp_pattern) && matches.size() >= 7) {
        info.PLL_Temperature_C = std::stof(matches[1]);
        info.CPU_Temperature_C = std::stof(matches[2]);
        info.PMIC_Temperature_C = std::stof(matches[3]);
        info.GPU_Temperature_C = std::stof(matches[4]);
        info.AO_Temperature_C = std::stof(matches[5]);
        info.Thermal_Temperature_C = std::stof(matches[6]);
        foundSomething = true;
    } else if (config_.partialParseAllowed) {
        // [PROD] A silent temperature-parse failure reproduces F26 (zero
        // temps -> thermal objective dark) on any future JetPack format
        // change. Debug-level partial-parse logging is not enough here.
        static std::atomic<uint64_t> tempParseFails{0};
        const auto nFails = ++tempParseFails;
        if (nFails == 1 || nFails % 60 == 0) {
            spdlog::warn("[SoCConcrete] Temperature fields NOT parsed ({}x) - "
                         "cpu_temp_c/gpu_temp_c will be 0 downstream. Line: {}",
                         nFails, output);
        }
    }

    if (!foundSomething && !config_.partialParseAllowed) {
        reportError("[SoCConcrete] parseTegraStats: no fields parsed, check tegrastats format or partialParseAllowed");
    }

    return info;
}

inline JetsonNanoInfo SoCConcrete::parseTx2Stats(const std::string& output) {
    if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] parseTx2Stats not implemented, returning partial info");
    }
    return parseTegraStats(output);
}

inline JetsonNanoInfo SoCConcrete::parseXavierStats(const std::string& output) {
    if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] parseXavierStats not implemented, returning partial info");
    }
    return parseTegraStats(output);
}

inline JetsonNanoInfo SoCConcrete::parseRpiStats(const std::string& output) {
    if (config_.partialParseAllowed) {
        logPartialParse("[SoCConcrete] parseRpiStats not implemented, returning partial info");
    }
    return parseTegraStats(output);
}

inline void SoCConcrete::logPartialParse(const std::string& msg) {
    spdlog::debug(msg);
}

inline void SoCConcrete::exportToCSV(const JetsonNanoInfo& info, const std::string& filePath) {
    try {
        std::ofstream file(filePath, std::ios::app);
        auto now_c = std::chrono::system_clock::to_time_t(info.timestamp);

        if (file.tellp() == 0) {
            file << "Timestamp,RAM_Used_MB,Total_RAM_MB,"
                 << "LFB_Size_MB,Block_Max_MB,"
                 << "SWAP_In_Use_MB,Total_SWAP_MB,Cached_MB,"
                 << "used_IRAM_kB,total_IRAM_kB,lfb_kB,"
                 << "CPU1_Util,CPU1_Freq_MHZ,CPU2_Util,CPU2_Freq_MHZ,"
                 << "CPU3_Util,CPU3_Freq_MHZ,CPU4_Util,CPU4_Freq_MHZ,"
                 << "EMC_Frequency_Percent,GR3D_Frequency_Percent,"
                 << "PLL_Temperature_C,CPU_Temperature_C,PMIC_Temperature_C,"
                 << "GPU_Temperature_C,AO_Temperature_C,Thermal_Temperature_C\n";
        }

        auto formatCPU = [](int util, int freq) -> std::string {
            return (util == 0 && freq == 0) ? "off,off" : std::to_string(util) + "," + std::to_string(freq);
        };

        file << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S") << ","
             << info.RAM_In_Use_MB << "," << info.Total_RAM_MB << ","
             << info.LFB_Size_MB << "," << info.Block_Max_MB << ","
             << info.SWAP_In_Use_MB << "," << info.Total_SWAP_MB << "," << info.Cached_MB << ","
             << info.used_IRAM_kB << "," << info.total_IRAM_kB << "," << info.lfb_kB << ","
             << formatCPU(info.CPU1_Utilization_Percent, info.CPU1_Frequency_MHz) << ","
             << formatCPU(info.CPU2_Utilization_Percent, info.CPU2_Frequency_MHz) << ","
             << formatCPU(info.CPU3_Utilization_Percent, info.CPU3_Frequency_MHz) << ","
             << formatCPU(info.CPU4_Utilization_Percent, info.CPU4_Frequency_MHz) << ","
             << info.EMC_Frequency_Percent << "," << info.GR3D_Frequency_Percent << ","
             << info.PLL_Temperature_C << "," << info.CPU_Temperature_C << ","
             << info.PMIC_Temperature_C << "," << info.GPU_Temperature_C << ","
             << info.AO_Temperature_C << "," << info.Thermal_Temperature_C << "\n";
    } catch (...) {
        reportError("[SoCConcrete] Failed to write CSV data");
    }
}

inline void SoCConcrete::reportError(const std::string& msg) {
    if (errorCallback_) {
        errorCallback_(msg);
    } else {
        spdlog::error("[SoCConcrete] {}", msg);
    }
}


//===============================================================================================
// // ==================================25-05-2025=====================================

// #pragma once

// //#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include "../Interfaces/ISoC.h"
// #include "../SharedStructures/SoCConfig.h"
// #include "../Interfaces/ISystemMetricsAggregator.h"
// #include "../SharedStructures/allModulesStatcs.h"

// #include <memory>
// #include <thread>
// #include <atomic>
// #include <mutex>
// #include <string>
// #include <vector>
// #include <chrono>
// #include <functional>
// #include <spdlog/spdlog.h>
// #include <cstdio>
// #include <regex>
// #include <array>    // [PROD] Explicit std::array header
// #include <fstream>
// #include <iomanip>
// #include <random> 

// class SoCConcrete : public ISoC {
// public:
//     SoCConcrete(std::shared_ptr<ISystemMetricsAggregator> aggregator);
//     ~SoCConcrete() override;

//     bool initializeSoC() override;
//     std::string getSpecs() override;
//     JetsonNanoInfo getPerformance() const override;
//     void stopSoC() override;

//     bool configure(const SoCConfig& config) override;
//     void setErrorCallback(std::function<void(const std::string&)> cb) override;
//     void pushSoCStats(const JetsonNanoInfo& stats) override;
//     void pauseSoC();
//     void resumeSoC();
//     bool isMonitoringActive() const; 

// private:
//     void monitorPerformance();
//     std::string getSoCInformationLine();
//     JetsonNanoInfo parseTegraStats(const std::string& output);
//     JetsonNanoInfo parseTx2Stats(const std::string& output);
//     JetsonNanoInfo parseXavierStats(const std::string& output);
//     JetsonNanoInfo parseRpiStats(const std::string& output);
//     void logPartialParse(const std::string& msg);
//     void exportToCSV(const JetsonNanoInfo& info, const std::string& filePath);
//     void reportError(const std::string& msg);
//     bool verifyTegraStatsOutput(); 

//     std::unique_ptr<FILE, decltype(&pclose)> pipe_{nullptr, pclose};
//     std::string command_;
//     SoCConfig config_;
//     std::thread monitoringThread_;
//     std::atomic<bool> monitoringActive_{false};
//     std::atomic<bool> monitoringPaused_{false};
//     mutable std::mutex performanceMutex_;
//     JetsonNanoInfo lastPerformanceData_;
//     std::function<void(const std::string&)> errorCallback_;
//     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;
// };

// inline SoCConcrete::SoCConcrete(std::shared_ptr<ISystemMetricsAggregator> aggregator)
//     : pipe_(nullptr, pclose), metricAggregator_(std::move(aggregator)) {}

// inline SoCConcrete::~SoCConcrete() {
//     stopSoC();
// }

// inline bool SoCConcrete::configure(const SoCConfig& config) {
//     config_ = config;
//     if (config_.customCommand.empty()) {
//         config_.customCommand = "tegrastats --interval " + std::to_string(config_.pollIntervalMs);
//     }
    
//     // [PROD] Keep tegrastats' emission cadence coupled to pollIntervalMs:
//     // This loop consumes ONE line per poll, so a faster producer grows the
//     // pipe buffer without bound and pushed samples lag wall time by many
//     // seconds while carrying fresh parse-time timestamps - silently
//     // misaligning SoC/temperature data inside the aggregator's windows.
//     if (config_.customCommand.find("tegrastats") != std::string::npos &&
//         config_.customCommand.find("--interval") == std::string::npos) {
//         config_.customCommand += " --interval " + std::to_string(config_.pollIntervalMs);
//     }

//     spdlog::info("[SoCConcrete] Configured SoC with command='{}', pollIntervalMs={}, exportCSV={}, boardType={}",
//                  config_.customCommand, config_.pollIntervalMs, config_.exportCSV,
//                  static_cast<int>(config_.boardType));
//     return true;
// }

// inline bool SoCConcrete::initializeSoC() {
//     if (monitoringActive_) {
//         spdlog::warn("[SoCConcrete] SoC is already initialized and running.");
//         return true;
//     }

//     try {
//         command_ = config_.customCommand;
//         pipe_ = std::unique_ptr<FILE, decltype(&pclose)>(
//             popen(command_.c_str(), "r"),
//             pclose
//         );
//         if (!pipe_) {
//             throw std::runtime_error("Could not popen() command: " + command_);
//         }

//         if (!verifyTegraStatsOutput()) {
//             pipe_.reset();
//             throw std::runtime_error("tegrastats command failed to produce valid output");
//         }

//         monitoringActive_ = true;
//         monitoringPaused_ = false;
//         monitoringThread_ = std::thread(&SoCConcrete::monitorPerformance, this);

//         std::this_thread::sleep_for(std::chrono::milliseconds(100));
//         if (!monitoringActive_) {
//             pipe_.reset();
//             monitoringThread_.join();
//             throw std::runtime_error("Monitoring thread failed to start");
//         }

//         spdlog::info("[SoCConcrete] SoC monitoring started with command '{}'.", command_);
//         return true;
//     }
//     catch (const std::exception& ex) {
//         monitoringActive_ = false;
//         pipe_.reset();
//         if (monitoringThread_.joinable()) {
//             monitoringThread_.join();
//         }
//         reportError("[SoCConcrete] Initialization failed: " + std::string(ex.what()));
//         return false;
//     }
// }

// inline bool SoCConcrete::verifyTegraStatsOutput() {
//     std::string line = getSoCInformationLine();
//     if (line.empty()) {
//         reportError("[SoCConcrete] No data from tegrastats command");
//         return false;
//     }

//     static const std::regex ram_pattern(R"(RAM\s+\d+/\d+MB)");
//     std::smatch matches;
//     if (!std::regex_search(line, matches, ram_pattern)) {
//         reportError("[SoCConcrete] tegrastats output invalid: missing RAM pattern");
//         return false;
//     }

//     return true;
// }

// inline void SoCConcrete::stopSoC() {
//     spdlog::info("[SoCConcrete] Stopping SoC monitoring...");

//     monitoringActive_ = false;
//     monitoringPaused_ = false;

//     if (monitoringThread_.joinable()) {
//         monitoringThread_.join();
//     }

//     if (pipe_) {
//         spdlog::info("[SoCConcrete] Closing tegrastats pipe.");
//         int closeRet = pclose(pipe_.release());
//         if (closeRet != 0) {
//             spdlog::error("[SoCConcrete] pclose failed with code {}", closeRet);
//         }
//     }

//     spdlog::info("[SoCConcrete] Stopped.");
// }

// inline void SoCConcrete::pauseSoC() {
//     if (monitoringActive_) {
//         monitoringPaused_ = true;
//         spdlog::info("[SoCConcrete] SoC monitoring is now paused.");
//     }
// }

// inline void SoCConcrete::resumeSoC() {
//     if (monitoringActive_) {
//         monitoringPaused_ = false;
//         spdlog::info("[SoCConcrete] SoC monitoring is now resumed.");
//     }
// }

// inline std::string SoCConcrete::getSpecs() {
//     switch (config_.boardType) {
//         case BoardType::JetsonNano:
//             return "Jetson Nano: Quad-core ARM Cortex-A57 @ 1.43 GHz, 128-core Maxwell GPU, 4GB LPDDR4";
//         case BoardType::JetsonTX2:
//             return "Jetson TX2: NVIDIA Denver2 + ARM A57 CPU, 256-core Pascal GPU, 8GB LPDDR4";
//         case BoardType::JetsonXavier:
//             return "Jetson Xavier: 8-core ARM v8.2 64-bit, 512-core Volta GPU, 16GB LPDDR4";
//         case BoardType::RaspberryPi:
//             return "Raspberry Pi: e.g., 4-core ARM Cortex-A72, VideoCore VI GPU, up to 4GB LPDDR4";
//         default:
//             return "Unknown board type: limited specs available.";
//     }
// }

// inline JetsonNanoInfo SoCConcrete::getPerformance() const {
//     std::lock_guard<std::mutex> lock(performanceMutex_);
//     return lastPerformanceData_;
// }

// inline void SoCConcrete::pushSoCStats(const JetsonNanoInfo& stats) {
//     // [F26-FIX] Direct forward to aggregator ensures thermal and energy metrics reach system snapshot.
//     // [F26-FIX+] Scoped mutex lock restores local cache so getPerformance() remains functional.
//     {
//         std::lock_guard<std::mutex> lock(performanceMutex_);
//         lastPerformanceData_ = stats;
//     }

//     if (metricAggregator_) {
//         metricAggregator_->pushSoCStats(stats);
//     }
// }

// inline void SoCConcrete::setErrorCallback(std::function<void(const std::string&)> cb) {
//     errorCallback_ = std::move(cb);
// }

// inline bool SoCConcrete::isMonitoringActive() const {
//     return monitoringActive_.load();
// }

// inline void SoCConcrete::monitorPerformance() {
//     spdlog::debug("[SoCConcrete] monitorPerformance() thread started.");

//     while (monitoringActive_) {
//         if (monitoringPaused_) {
//             std::this_thread::sleep_for(std::chrono::milliseconds(500));
//             continue;
//         }

//         std::string line = getSoCInformationLine();
//         if (!line.empty()) {
//             JetsonNanoInfo info;
//             switch (config_.boardType) {
//                 case BoardType::JetsonNano:
//                     info = parseTegraStats(line);
//                     break;
//                 case BoardType::JetsonTX2:
//                     info = parseTx2Stats(line);
//                     break;
//                 case BoardType::JetsonXavier:
//                     info = parseXavierStats(line);
//                     break;
//                 case BoardType::RaspberryPi:
//                     info = parseRpiStats(line);
//                     break;
//                 default:
//                     info = parseTegraStats(line);
//                     break;
//             }
//             pushSoCStats(info);
//             if (config_.exportCSV) {
//                 exportToCSV(info, config_.csvPath);
//             }
//         } else {
//             reportError("[SoCConcrete] No data from SoC command. Retrying...");
//             std::this_thread::sleep_for(std::chrono::milliseconds(500));
//             continue;
//         }

//         std::this_thread::sleep_for(std::chrono::milliseconds(config_.pollIntervalMs));
//     }
//     pipe_.reset();
//     spdlog::debug("[SoCConcrete] monitorPerformance() thread exiting.");
// }

// inline std::string SoCConcrete::getSoCInformationLine() {
//     if (!pipe_) {
//         return {};
//     }
//     std::array<char, 512> buffer;
//     if (fgets(buffer.data(), buffer.size(), pipe_.get()) != nullptr) {
//         return std::string(buffer.data());
//     }
//     return {};
// }

// inline JetsonNanoInfo SoCConcrete::parseTegraStats(const std::string& output) {
//     JetsonNanoInfo info;
//     info.timestamp = std::chrono::system_clock::now();

//     static const std::regex ram_pattern(R"(RAM\s+(\d+)/(\d+)MB)");
//     static const std::regex lfb_pattern(R"(lfb\s+(\d+)x(\d+)MB)");
//     static const std::regex swap_pattern(R"(SWAP\s+(\d+)/(\d+)MB)");
//     static const std::regex cached_pattern(R"(cached\s+(\d+)MB)");
//     static const std::regex iram_pattern(R"(IRAM\s+(\d+)/(\d+)kB\s*\(lfb\s+(\d+)kB\))");
//     static const std::regex cpu_pattern(
//         R"(CPU\s+\[(?:(\d+)%@(\d+)|off),(?:(\d+)%@(\d+)|off),(?:(\d+)%@(\d+)|off),(?:(\d+)%@(\d+)|off)\])");
//    // static const std::regex emcgr3d_freq_pattern(R"(EMC_FREQ\s+(\d+)%@[\d]+\s*GR3D_FREQ\s+(\d+)%@[\d]+)");
//    // [TELEMETRY-FIX3] Parse EMC and GR3D independently.
//     // Jetson Nano tegrastats variants may emit either:
//     //   EMC_FREQ 12% GR3D_FREQ 37%
//     // or:
//     //   EMC_FREQ 12%@1600 GR3D_FREQ 37%@921
//     // We only need the utilization percentages here, so do not require @frequency.
//     static const std::regex emc_freq_pattern(R"(EMC_FREQ\s+(\d+)%)");
//     static const std::regex gr3d_freq_pattern(R"(GR3D_FREQ\s+(\d+)%)");

//     static const std::regex temp_pattern(R"(PLL@([\d.]+)C\s+CPU@([\d.]+)C\s+PMIC@([\d.]+)C\s+GPU@([\d.]+)C\s+AO@([\d.]+)C\s+thermal@([\d.]+)C)");

//     std::smatch matches;
//     bool foundSomething = false;

//     if (std::regex_search(output, matches, ram_pattern) && matches.size() >= 3) {
//         info.RAM_In_Use_MB = std::stoi(matches[1]);
//         info.Total_RAM_MB = std::stoi(matches[2]);
//         foundSomething = true;
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse RAM from line: " + output);
//     }

//     if (std::regex_search(output, matches, lfb_pattern) && matches.size() >= 3) {
//         info.LFB_Size_MB = std::stoi(matches[1]);
//         info.Block_Max_MB = std::stoi(matches[2]);
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse LFB from line: " + output);
//     }

//     if (std::regex_search(output, matches, swap_pattern) && matches.size() >= 3) {
//         info.SWAP_In_Use_MB = std::stoi(matches[1]);
//         info.Total_SWAP_MB = std::stoi(matches[2]);
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse SWAP from line: " + output);
//     }

//     if (std::regex_search(output, matches, cached_pattern) && matches.size() >= 2) {
//         info.Cached_MB = std::stoi(matches[1]);
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse CACHE from line: " + output);
//     }

//     if (std::regex_search(output, matches, iram_pattern) && matches.size() >= 4) {
//         info.used_IRAM_kB = std::stoi(matches[1]);
//         info.total_IRAM_kB = std::stoi(matches[2]);
//         info.lfb_kB = std::stoi(matches[3]);
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse IRAM from line: " + output);
//     }

//     if (std::regex_search(output, matches, cpu_pattern) && matches.size() >= 9) {
//         auto parseCpu = [](const std::string& utilStr, const std::string& freqStr, double& util, double& freq) {
//             if (utilStr.empty() || freqStr.empty()) {
//                 util = 0;
//                 freq = 0;
//             } else {
//                 util = std::stoi(utilStr);
//                 freq = std::stoi(freqStr);
//             }
//         };
//         parseCpu(matches[1], matches[2], info.CPU1_Utilization_Percent, info.CPU1_Frequency_MHz);
//         parseCpu(matches[3], matches[4], info.CPU2_Utilization_Percent, info.CPU2_Frequency_MHz);
//         parseCpu(matches[5], matches[6], info.CPU3_Utilization_Percent, info.CPU3_Frequency_MHz);
//         parseCpu(matches[7], matches[8], info.CPU4_Utilization_Percent, info.CPU4_Frequency_MHz);
//         foundSomething = true;
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse CPU usage from line: " + output);
//     }

//     if (std::regex_search(output, matches, emcgr3d_freq_pattern) && matches.size() >= 3) {
//         info.EMC_Frequency_Percent = std::stoi(matches[1]);
//         info.GR3D_Frequency_Percent = std::stoi(matches[2]);
//     } else if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] Could not parse EMC/GR3D from line: " + output);
//     }

//     if (std::regex_search(output, matches, temp_pattern) && matches.size() >= 7) {
//         info.PLL_Temperature_C = std::stof(matches[1]);
//         info.CPU_Temperature_C = std::stof(matches[2]);
//         info.PMIC_Temperature_C = std::stof(matches[3]);
//         info.GPU_Temperature_C = std::stof(matches[4]);
//         info.AO_Temperature_C = std::stof(matches[5]);
//         info.Thermal_Temperature_C = std::stof(matches[6]);
//         foundSomething = true;
//     } else if (config_.partialParseAllowed) {
//         // [PROD] A silent temperature-parse failure reproduces F26 (zero
//         // temps -> thermal objective dark) on any future JetPack format
//         // change. Debug-level partial-parse logging is not enough here.
//         static std::atomic<uint64_t> tempParseFails{0};
//         const auto nFails = ++tempParseFails;
//         if (nFails == 1 || nFails % 60 == 0) {
//             spdlog::warn("[SoCConcrete] Temperature fields NOT parsed ({}x) - "
//                          "cpu_temp_c/gpu_temp_c will be 0 downstream. Line: {}",
//                          nFails, output);
//         }
//     }

//     if (!foundSomething && !config_.partialParseAllowed) {
//         reportError("[SoCConcrete] parseTegraStats: no fields parsed, check tegrastats format or partialParseAllowed");
//     }

//     return info;
// }

// inline JetsonNanoInfo SoCConcrete::parseTx2Stats(const std::string& output) {
//     if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] parseTx2Stats not implemented, returning partial info");
//     }
//     return parseTegraStats(output);
// }

// inline JetsonNanoInfo SoCConcrete::parseXavierStats(const std::string& output) {
//     if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] parseXavierStats not implemented, returning partial info");
//     }
//     return parseTegraStats(output);
// }

// inline JetsonNanoInfo SoCConcrete::parseRpiStats(const std::string& output) {
//     if (config_.partialParseAllowed) {
//         logPartialParse("[SoCConcrete] parseRpiStats not implemented, returning partial info");
//     }
//     return parseTegraStats(output);
// }

// inline void SoCConcrete::logPartialParse(const std::string& msg) {
//     spdlog::debug(msg);
// }

// inline void SoCConcrete::exportToCSV(const JetsonNanoInfo& info, const std::string& filePath) {
//     try {
//         std::ofstream file(filePath, std::ios::app);
//         auto now_c = std::chrono::system_clock::to_time_t(info.timestamp);

//         if (file.tellp() == 0) {
//             file << "Timestamp,RAM_Used_MB,Total_RAM_MB,"
//                  << "LFB_Size_MB,Block_Max_MB,"
//                  << "SWAP_In_Use_MB,Total_SWAP_MB,Cached_MB,"
//                  << "used_IRAM_kB,total_IRAM_kB,lfb_kB,"
//                  << "CPU1_Util,CPU1_Freq_MHZ,CPU2_Util,CPU2_Freq_MHZ,"
//                  << "CPU3_Util,CPU3_Freq_MHZ,CPU4_Util,CPU4_Freq_MHZ,"
//                  << "EMC_Frequency_Percent,GR3D_Frequency_Percent,"
//                  << "PLL_Temperature_C,CPU_Temperature_C,PMIC_Temperature_C,"
//                  << "GPU_Temperature_C,AO_Temperature_C,Thermal_Temperature_C\n";
//         }

//         auto formatCPU = [](int util, int freq) -> std::string {
//             return (util == 0 && freq == 0) ? "off,off" : std::to_string(util) + "," + std::to_string(freq);
//         };

//         file << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S") << ","
//              << info.RAM_In_Use_MB << "," << info.Total_RAM_MB << ","
//              << info.LFB_Size_MB << "," << info.Block_Max_MB << ","
//              << info.SWAP_In_Use_MB << "," << info.Total_SWAP_MB << "," << info.Cached_MB << ","
//              << info.used_IRAM_kB << "," << info.total_IRAM_kB << "," << info.lfb_kB << ","
//              << formatCPU(info.CPU1_Utilization_Percent, info.CPU1_Frequency_MHz) << ","
//              << formatCPU(info.CPU2_Utilization_Percent, info.CPU2_Frequency_MHz) << ","
//              << formatCPU(info.CPU3_Utilization_Percent, info.CPU3_Frequency_MHz) << ","
//              << formatCPU(info.CPU4_Utilization_Percent, info.CPU4_Frequency_MHz) << ","
//              << info.EMC_Frequency_Percent << "," << info.GR3D_Frequency_Percent << ","
//              << info.PLL_Temperature_C << "," << info.CPU_Temperature_C << ","
//              << info.PMIC_Temperature_C << "," << info.GPU_Temperature_C << ","
//              << info.AO_Temperature_C << "," << info.Thermal_Temperature_C << "\n";
//     } catch (...) {
//         reportError("[SoCConcrete] Failed to write CSV data");
//     }
// }

// inline void SoCConcrete::reportError(const std::string& msg) {
//     if (errorCallback_) {
//         errorCallback_(msg);
//     } else {
//         spdlog::error("[SoCConcrete] {}", msg);
//     }
// }
// //=============================================================================