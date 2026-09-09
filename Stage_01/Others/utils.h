// utils.h
#pragma once

#include <chrono>
#include <sstream>
#include <iomanip>
#include <string>
#include <ctime>

namespace utils {

//==========================================================================
// Helper: Safe Clamping (C++11+ replacement for std::clamp)
// - Put it in utils:: so it never collides with other headers.
// - Call it explicitly as utils::local_clamp(...)
//==========================================================================
template<typename T>
inline constexpr T local_clamp(const T& v, const T& lo, const T& hi) {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

/**
 * @brief Utility function to format timestamp as a string.
 * @param tp Timestamp to format.
 * @return Formatted string (YYYY-MM-DD HH:MM:SS.mmm).
 */
inline std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
    using namespace std::chrono;

    const auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % milliseconds(1000);
    const std::time_t tt = system_clock::to_time_t(tp);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);   // Thread-safe on Windows
#else
    localtime_r(&tt, &tm);   // Thread-safe on POSIX
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count();

    return ss.str();
}

} // namespace utils
