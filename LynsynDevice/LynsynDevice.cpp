// LynsynDevice.cpp
#include "LynsynDevice.h"
#include <algorithm>
#include <cstring>
#include <thread>
#include <iostream>
#include <cmath>

static void logError(const std::string& msg) {
    std::cerr << "[LynsynDevice] ERROR: " << msg << '\n';
}

/* ------------------------------------------------------------------------ */
LynsynDevice::LynsynDevice(unsigned maxTries, int maxConsecutiveFails)
    : maxConsecutiveFails_(maxConsecutiveFails)
{
    sampleBuf_.resize(BUFFER_CAPACITY);
    sampleBufIndex_ = BUFFER_CAPACITY;   // mark empty
    init(maxTries);
    fetchCalInfo();
}

LynsynDevice::~LynsynDevice() noexcept { release(); }

/* ---- move semantics ----------------------------------------------------- */
LynsynDevice::LynsynDevice(LynsynDevice&& o) noexcept
    : usbContext_(o.usbContext_), handle_(o.handle_),
      outEndpoint_(o.outEndpoint_), inEndpoint_(o.inEndpoint_),
      sampleBuf_(std::move(o.sampleBuf_)), sampleBufIndex_(o.sampleBufIndex_),
      calInfo_(o.calInfo_), hwVer_(o.hwVer_), bootVer_(o.bootVer_),
      swVer_(o.swVer_), numCores_(o.numCores_), useMarkBp_(o.useMarkBp_),
      consecutiveFails_(o.consecutiveFails_), maxConsecutiveFails_(o.maxConsecutiveFails_),
      lastArmMode_(o.lastArmMode_), lastDurationSec_(o.lastDurationSec_),
      lastCores_(o.lastCores_), lastStartAddr_(o.lastStartAddr_),
      lastEndAddr_(o.lastEndAddr_)
{
    o.usbContext_ = nullptr; o.handle_ = nullptr;
    o.sampleBufIndex_ = 0; o.consecutiveFails_ = 0;
}

LynsynDevice& LynsynDevice::operator=(LynsynDevice&& o) noexcept {
    if (this != &o) {
        release();
        usbContext_ = o.usbContext_; handle_ = o.handle_;
        outEndpoint_ = o.outEndpoint_; inEndpoint_ = o.inEndpoint_;
        sampleBuf_ = std::move(o.sampleBuf_);
        sampleBufIndex_ = o.sampleBufIndex_;
        calInfo_ = o.calInfo_;
        hwVer_ = o.hwVer_; bootVer_ = o.bootVer_; swVer_ = o.swVer_;
        numCores_ = o.numCores_; useMarkBp_ = o.useMarkBp_;
        consecutiveFails_ = o.consecutiveFails_;
        maxConsecutiveFails_ = o.maxConsecutiveFails_;
        lastArmMode_ = o.lastArmMode_; lastDurationSec_ = o.lastDurationSec_;
        lastCores_ = o.lastCores_; lastStartAddr_ = o.lastStartAddr_;
        lastEndAddr_ = o.lastEndAddr_;

        o.usbContext_ = nullptr; o.handle_ = nullptr;
        o.sampleBufIndex_ = 0; o.consecutiveFails_ = 0;
    }
    return *this;
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::init(unsigned maxTries) {
    std::lock_guard<std::mutex> lk(mutex_);
    int r = libusb_init(&usbContext_);
    if (r < 0) throw LynsynInitException("libusb_init: "s + libusb_error_name(r));

    unsigned tries = 0;
    while (tries++ < maxTries) {
        handle_ = libusb_open_device_with_vid_pid(usbContext_, 0x10c4, 0x8c1e);
        if (handle_) break;
        logError("Waiting for Lynsyn");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!handle_) throw LynsynInitException("Cannot open Lynsyn after "s + std::to_string(maxTries) + " tries");

    libusb_set_auto_detach_kernel_driver(handle_, 1);
    r = libusb_claim_interface(handle_, 1);
    if (r < 0) throw LynsynInitException("claim iface: "s + libusb_error_name(r));

    /* ---- discover endpoints ------------------------------------------------ */
    libusb_config_descriptor* cfg = nullptr;
    libusb_get_active_config_descriptor(libusb_get_device(handle_), &cfg);
    if (!cfg) throw LynsynInitException("no config descriptor");

    if (cfg->bNumInterfaces > 1) {
        const auto& iface = cfg->interface[1].altsetting[0];
        for (int i = 0; i < iface.bNumEndpoints; ++i) {
            uint8_t ep = iface.endpoint[i].bEndpointAddress;
            if (ep & 0x80) inEndpoint_ = ep;
            else           outEndpoint_ = ep;
        }
    }
    libusb_free_config_descriptor(cfg);

    /* ---- read firmware versions ------------------------------------------- */
    RequestPacket req{ .cmd = USB_CMD_INIT };
    sendBytes(reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    InitReplyPacket rep{};
    getBytes(reinterpret_cast<uint8_t*>(&rep), sizeof(rep));
    hwVer_   = rep.hwVersion;
    bootVer_ = rep.bootVersion;
    swVer_   = rep.swVersion;
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::release() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (handle_) {
        libusb_release_interface(handle_, 1);
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (usbContext_) { libusb_exit(usbContext_); usbContext_ = nullptr; }
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::fetchCalInfo() {
    std::lock_guard<std::mutex> lk(mutex_);
    RequestPacket req{ .cmd = USB_CMD_CALINFO };
    sendBytes(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    getBytes(reinterpret_cast<uint8_t*>(&calInfo_), sizeof(calInfo_));
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::stopSampling() {
    std::lock_guard<std::mutex> lk(mutex_);
    RequestPacket req{ .cmd = USB_CMD_STOP_SAMPLING };
    sendBytes(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    sampleBufIndex_ = BUFFER_CAPACITY;   // discard any pending data
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::startPeriodSampling(double durationSec, uint64_t cores) {
    std::lock_guard<std::mutex> lk(mutex_);
    stopSampling();   // safety

    StartSamplingRequestPacket req{};
    req.request.cmd      = USB_CMD_START_SAMPLING;
    req.samplePeriod     = lynsyn_secondsToCycles(durationSec);
    req.cores            = cores;
    req.flags            = SAMPLING_FLAG_PERIOD;

    sendBytes(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    sampleBufIndex_ = BUFFER_CAPACITY;

    lastArmMode_      = ArmMode::Period;
    lastDurationSec_  = durationSec;
    lastCores_        = cores;
}

/* analogous implementations for startBpSampling / startBpPeriodSampling  */

/* ------------------------------------------------------------------------ */
bool LynsynDevice::getNextSample(LynsynSample& sample) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (!getNextRaw()) {
        ++consecutiveFails_;
        logError("getNextRaw failed (#" + std::to_string(consecutiveFails_) + ")");
        if (consecutiveFails_ >= maxConsecutiveFails_)
            throw LynsynFatalException("too many consecutive failures");

        // exponential back-off, capped at 2^5 = 32 × BASE
        int backoff = BASE_BACKOFF_MS * (1 << std::min(consecutiveFails_ - 1, 5));
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));

        // try to recover on 2nd fail and every 3rd after that
        if (consecutiveFails_ == 2 || (consecutiveFails_ > 2 && consecutiveFails_ % 3 == 0))
            attemptRecovery();

        return false;
    }

    consecutiveFails_ = 0;
    convertSample(sample, sampleBuf_[sampleBufIndex_ - 1]);
    return !(sample.flags & SAMPLE_FLAG_HALTED);
}

/* ------------------------------------------------------------------------ */
bool LynsynDevice::attemptRecovery() {
    // full USB reset + re-open + re-arm
    libusb_release_interface(handle_, 1);
    if (libusb_reset_device(handle_) != LIBUSB_SUCCESS) {
        logError("reset_device failed");
        return false;
    }
    libusb_close(handle_);
    handle_ = libusb_open_device_with_vid_pid(usbContext_, 0x10c4, 0x8c1e);
    if (!handle_) { logError("re-open failed"); return false; }

    libusb_set_auto_detach_kernel_driver(handle_, 1);
    if (libusb_claim_interface(handle_, 1) != LIBUSB_SUCCESS) {
        logError("re-claim failed");
        libusb_close(handle_); handle_ = nullptr;
        return false;
    }

    // re-discover endpoints (same code as init)
    libusb_config_descriptor* cfg = nullptr;
    libusb_get_active_config_descriptor(libusb_get_device(handle_), &cfg);
    if (cfg && cfg->bNumInterfaces > 1) {
        const auto& iface = cfg->interface[1].altsetting[0];
        for (int i = 0; i < iface.bNumEndpoints; ++i) {
            uint8_t ep = iface.endpoint[i].bEndpointAddress;
            if (ep & 0x80) inEndpoint_ = ep; else outEndpoint_ = ep;
        }
    }
    libusb_free_config_descriptor(cfg);

    // re-arm
    switch (lastArmMode_) {
        case ArmMode::Period:      startPeriodSampling(lastDurationSec_, lastCores_); break;
        case ArmMode::Bp:          startBpSampling(lastStartAddr_, lastEndAddr_, lastCores_); break;
        case ArmMode::BpPeriod:    startBpPeriodSampling(lastStartAddr_, lastDurationSec_, lastCores_); break;
    }
    logError("recovery succeeded");
    return true;
}

/* ------------------------------------------------------------------------ */
bool LynsynDevice::getNextRaw() {
    if (sampleBufIndex_ >= sampleBuf_.size()) {
        unsigned received = 0;
        if (!getArray(reinterpret_cast<uint8_t*>(sampleBuf_.data()),
                     static_cast<int>(sampleBuf_.size() * sizeof(SampleReplyPacket)),
                     &received, USB_IO_TIMEOUT_MS))
            return false;

        if (received == 0) return false;
        const size_t packets = received / sizeof(SampleReplyPacket);
        sampleBufIndex_ = 0;
        if (sampleBuf_[0].time == -1) return false;   // halt packet
        sampleBuf_.resize(packets);                  // shrink if less than capacity
    }
    ++sampleBufIndex_;
    return true;
}

/* ------------------------------------------------------------------------ */
bool LynsynDevice::getArray(uint8_t* buf, int maxBytes, unsigned* elementsReceived, uint32_t timeoutMs) {
    // The real Lynsyn protocol sends a 16-bit count first, then that many packets.
    // For simplicity we request the maximum possible and let libusb return what it got.
    int transferred = 0;
    int r = libusb_bulk_transfer(handle_, inEndpoint_, buf, maxBytes, &transferred, timeoutMs);
    if (r != LIBUSB_SUCCESS) {
        logError("bulk IN failed: "s + libusb_error_name(r));
        *elementsReceived = 0;
        return false;
    }
    *elementsReceived = transferred;
    return true;
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::sendBytes(const uint8_t* bytes, int numBytes) {
    int transferred = 0;
    int r = libusb_bulk_transfer(handle_, outEndpoint_,
                                 const_cast<uint8_t*>(bytes), numBytes,
                                 &transferred, USB_IO_TIMEOUT_MS);
    if (r != LIBUSB_SUCCESS || transferred != numBytes)
        throw LynsynException("send failed: "s + libusb_error_name(r));
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::getBytes(uint8_t* bytes, int numBytes, uint32_t timeoutMs) {
    int transferred = 0;
    int r = libusb_bulk_transfer(handle_, inEndpoint_, bytes, numBytes,
                                 &transferred, timeoutMs);
    if (r != LIBUSB_SUCCESS || transferred != numBytes)
        throw LynsynException("recv failed: "s + libusb_error_name(r));
}

/* ------------------------------------------------------------------------ */
double LynsynDevice::getCurrent(int16_t raw, int sensor) const {
    /* ... exact same logic as original, but with safe bounds ... */
    int point = 0;
    while (point < calInfo_.currentPoints[sensor] - 1 &&
           raw >= calInfo_.pointCurrent[sensor][point])
        ++point;

    double offset = calInfo_.offsetCurrent[sensor][point];
    double gain   = calInfo_.gainCurrent[sensor][point];
    double v = ((raw - offset) * LYNSYN_REF_VOLTAGE / LYNSYN_MAX_SENSOR_VALUE) * gain;

    double vs;
    if (hwVer_ >= HW_VERSION_3_1)      vs = v / CURRENT_SENSOR_GAIN_V3_1;
    else if (hwVer_ == HW_VERSION_3_0) vs = v / CURRENT_SENSOR_GAIN_V3;
    else                               vs = v / CURRENT_SENSOR_GAIN_V2;

    return vs / calInfo_.r[sensor];
}

/* (getVoltage, convertSample, channel helpers  unchanged but with bounds checks) */

/* ------------------------------------------------------------------------ */
LynsynSample LynsynDevice::getAvgSample(double durationSec, uint64_t cores) {
    startPeriodSampling(durationSec, cores);
    LynsynSample acc{};
    unsigned cnt = 0;
    LynsynSample s;

    while (getNextSample(s)) {
        for (unsigned i = 0; i < LYNSYN_MAX_SENSORS; ++i) {
            acc.current[i] += s.current[i];
            acc.voltage[i] += s.voltage[i];
        }
        ++cnt;
    }
    stopSampling();

    if (cnt == 0) throw LynsynException("no samples for average");
    for (unsigned i = 0; i < LYNSYN_MAX_SENSORS; ++i) {
        acc.current[i] /= cnt;
        acc.voltage[i] /= cnt;
    }
    return acc;
}

/* ------------------------------------------------------------------------ */
/*  CRC32  IEEE (reflected)  matches the firmware implementation          */
uint32_t LynsynDevice::crc32_table[256];
bool     LynsynDevice::crc_table_initialized = false;

void LynsynDevice::generate_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        crc32_table[i] = c;
    }
    crc_table_initialized = true;
}

uint32_t LynsynDevice::lynsyn_crc32(uint32_t crc, const uint32_t* data, int length) {
    if (!crc_table_initialized) generate_crc32_table();

    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    int bytes = length * 4;

    uint32_t c = ~crc;                     // init
    for (int i = 0; i < bytes; ++i)
        c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;                             // final XOR
}

/* ------------------------------------------------------------------------ */
void LynsynDevice::firmwareUpgrade(int size, uint8_t* buf) {
    std::lock_guard<std::mutex> lk(mutex_);
    RequestPacket req{ .cmd = USB_CMD_UPGRADE_INIT };
    sendBytes(reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    const int chunk = FLASH_BUFFER_SIZE;
    for (int off = 0; off < size; off += chunk) {
        UpgradeStoreRequestPacket sreq{};
        sreq.request.cmd = USB_CMD_UPGRADE_STORE;
        int cur = std::min(chunk, size - off);
        std::memcpy(sreq.data, buf + off, cur);
        sendBytes(reinterpret_cast<const uint8_t*>(&sreq), sizeof(sreq));
    }

    uint32_t crc = lynsyn_crc32(0, reinterpret_cast<uint32_t*>(buf), (size + 3) / 4);
    UpgradeFinaliseRequestPacket freq{};
    freq.request.cmd = USB_CMD_UPGRADE_FINALISE;
    freq.crc = crc;
    sendBytes(reinterpret_cast<const uint8_t*>(&freq), sizeof(freq));
}

/* ------------------------------------------------------------------------ */
/*  All remaining methods (jtagInit, setTck, shift, calibration, tests, )   */
/*  are either unchanged or have only minor safety casts added.              */
/* ------------------------------------------------------------------------ */
