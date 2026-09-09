#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <libusb-1.0/libusb.h>

#include "usbprotocol.h"
#include "lynsyn.h"

/* -------------------------------------------------------------------------- */
/*  LynsynDevice  modern C++ RAII wrapper around the Lynsyn USB board        */
/* -------------------------------------------------------------------------- */
class LynsynDevice {
public:
    /* ---- Construction ---------------------------------------------------- */
    explicit LynsynDevice(unsigned maxTries = 20, int maxConsecutiveFails = 10);
    ~LynsynDevice() noexcept;

    LynsynDevice(const LynsynDevice&) = delete;
    LynsynDevice& operator=(const LynsynDevice&) = delete;
    LynsynDevice(LynsynDevice&&) noexcept;
    LynsynDevice& operator=(LynsynDevice&&) noexcept;

    /* ---- High-level sampling API ------------------------------------------ */
    void startPeriodSampling(double durationSec, uint64_t cores);
    void startBpSampling(uint64_t startAddr, uint64_t endAddr, uint64_t cores);
    void startBpPeriodSampling(uint64_t startAddr, double durationSec, uint64_t cores);
    void stopSampling();                                   // NEW

    /** @return true if a valid sample was placed in *sample, false on halt or recoverable error */
    bool getNextSample(LynsynSample& sample);

    LynsynSample getSample(bool average = false, uint64_t cores = 0);
    LynsynSample getAvgSample(double durationSec, uint64_t cores = 0);

    void setMarkBreakpoint(uint64_t addr);

    /* ---- Device information ----------------------------------------------- */
    unsigned getNumCores() const noexcept   { return numCores_; }
    unsigned getNumSensors() const noexcept { return (hwVer_ >= HW_VERSION_3_0) ? 3u : 7u; }
    uint8_t  getHwVer() const noexcept      { return hwVer_; }

    bool getLog(char* buf, unsigned size);
    double getMaxCurrent(double rl) const;
    double getMaxVoltage()   const;

    const char* getVersionString(uint8_t version) const;

    /* ---- JTAG ------------------------------------------------------------- */
    void jtagInit(LynsynJtagDevice* devices);
    uint32_t setTck(uint32_t period);
    void shift(int numBits, uint8_t* tmsVector, uint8_t* tdiVector, uint8_t* tdoVector = nullptr);
    void trst(uint8_t val);

    /* ---- Calibration / Tests ---------------------------------------------- */
    void firmwareUpgrade(int size, uint8_t* buf);
    void cleanNonVolatile(uint8_t hwVersion, double* r);
    void adcCalibrateCurrent(uint8_t sensor, double current, double maxCurrent);
    void adcCalibrateVoltage(uint8_t sensor, double current, double maxCurrent);
    bool testUsb();
    bool testAdcCurrent(unsigned sensor, double val, double acceptance);
    bool testAdcVoltage(unsigned sensor, double val, double acceptance);
    void setLed(bool on);

private:
    /* ---- Internal helpers ------------------------------------------------- */
    void init(unsigned maxTries);
    void release();
    void fetchCalInfo();                                   // NEW
    bool attemptRecovery();
    void sendBytes(const uint8_t* bytes, int numBytes);
    void getBytes(uint8_t* bytes, int numBytes, uint32_t timeoutMs = 1000);
    bool getArray(uint8_t* bytes, int maxBytes, unsigned* elementsReceived, uint32_t timeoutMs = 1000);
    bool getNextRaw();
    void setBreakpoint(uint8_t type, uint64_t addr);
    double getCurrent(int16_t raw, int sensor) const;
    double getVoltage(int16_t raw, int sensor) const;
    void convertSample(LynsynSample& dest, const SampleReplyPacket& src) const;
    uint8_t getCurrentChannel(uint8_t sensor) const;
    uint8_t getVoltageChannel(uint8_t sensor) const;

    /* ---- State ------------------------------------------------------------ */
    libusb_context*        usbContext_ = nullptr;
    libusb_device_handle*  handle_     = nullptr;
    uint8_t                outEndpoint_ = 0;
    uint8_t                inEndpoint_  = 0;

    static constexpr size_t BUFFER_CAPACITY = 256;          // was MAX_SAMPLES
    std::vector<SampleReplyPacket> sampleBuf_;
    size_t                         sampleBufIndex_ = BUFFER_CAPACITY;

    CalInfoPacket calInfo_{};
    uint8_t       hwVer_ = 0;
    uint8_t       bootVer_ = 0;
    uint8_t       swVer_ = 0;
    unsigned      numCores_ = 0;
    bool          useMarkBp_ = false;

    mutable std::mutex mutex_;

    /* ---- Recovery state --------------------------------------------------- */
    int  consecutiveFails_ = 0;
    int  maxConsecutiveFails_ = 10;

    enum class ArmMode { Period, Bp, BpPeriod };
    ArmMode   lastArmMode_ = ArmMode::Period;
    double    lastDurationSec_ = 0.0;
    uint64_t  lastCores_ = 0;
    uint64_t  lastStartAddr_ = 0;
    uint64_t  lastEndAddr_   = 0;

    static constexpr uint32_t USB_IO_TIMEOUT_MS = 1000;
    static constexpr int      BASE_BACKOFF_MS   = 100;

    /* ---- CRC32 ----------------------------------------------------------- */
    static uint32_t lynsyn_crc32(uint32_t crc, const uint32_t* data, int length);
    static void     generate_crc32_table();
    static uint32_t crc32_table[256];
    static bool     crc_table_initialized;
};

/* -------------------------------------------------------------------------- */
/*  Minimal protocol constants (if not already in usbprotocol.h)              */
/* -------------------------------------------------------------------------- */
#ifndef HW_VERSION_3_0
enum { HW_VERSION_2_0 = 0x20, HW_VERSION_3_0 = 0x30, HW_VERSION_3_1 = 0x31 };
#endif
#ifndef SW_VERSION_2_2
enum { SW_VERSION_2_2 = 0x22 };
#endif