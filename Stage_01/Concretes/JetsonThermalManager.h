//===================================================================
//JetsonThermalManager.h
//===================================================================

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <filesystem>

namespace fs = std::filesystem;

class JetsonThermalManager {
public:
    struct ZoneSnapshot {
        std::string type_tag;
        double temp_c{0.0};
        bool valid{false};
    };

    // Auto-discovers all thermal zones available on the current hardware
    void discover_zones() {
        zone_paths_.clear();
        std::string sysfs_base = "/sys/class/thermal";
        
        if (!fs::exists(sysfs_base)) return;

        for (const auto& entry : fs::directory_iterator(sysfs_base)) {
            std::string path_str = entry.path().string();
            if (path_str.find("thermal_zone") != std::string::npos) {
                std::string type_file = path_str + "/type";
                std::ifstream t_stream(type_file);
                std::string type_name;
                if (t_stream >> type_name) {
                    zone_paths_[type_name] = path_str + "/temp";
                }
            }
        }
    }

    // Reads a specific zone by string tag (e.g., "CPU-therm", "GPU-therm", "PMIC-Die")
    double get_temperature(const std::string& type_tag) const {
        auto it = zone_paths_.find(type_tag);
        if (it == zone_paths_.end()) return -1.0;

        std::ifstream temp_stream(it->second);
        double raw_mC = 0.0;
        if (temp_stream >> raw_mC) {
            return raw_mC / 1000.0; // Standardize millidegrees to Celsius
        }
        return -1.0;
    }

    // Reads all registered zones into a tag-to-value map
    std::unordered_map<std::string, double> read_all_temperatures() const {
        std::unordered_map<std::string, double> snapshot;
        for (const auto& [tag, temp_path] : zone_paths_) {
            std::ifstream temp_stream(temp_path);
            double raw_mC = 0.0;
            if (temp_stream >> raw_mC) {
                snapshot[tag] = raw_mC / 1000.0;
            }
        }
        return snapshot;
    }

private:
    std::unordered_map<std::string, std::string> zone_paths_; // maps "CPU-therm" -> "/sys/.../temp"
};