#pragma once
#include <string>
//#include "../nlohmann/json.hpp" // Include nlohmann/json for JSON parsing
#include "../nlohmann/json.hpp"

struct RawDataConfig {
public:
    RawDataConfig()
        : inputSource("default_source"),
        videoFilePath("default_path") {}

    // Getters
    const std::string& getInputSource() const { return inputSource; }
    const std::string& getVideoFilePath() const { return videoFilePath; }

    // Setters
    void setInputSource(const std::string& source) { inputSource = source; }
    void setVideoFilePath(const std::string& path) { videoFilePath = path; }

    // From_json method for deserialization from JSON
    void from_json(const nlohmann::json& j) {
        j.at("source").get_to(this->inputSource);
        j.at("filePath").get_to(this->videoFilePath);
    }

private:
    std::string inputSource;    // e.g., "camera", "file", "network"
    std::string videoFilePath;   // path to a video file if inputSource == "file"
};
