
// #pragma once
// /**
//  * SdlDisplayConcrete_new.h - FINAL TARGETED FIX v3.3
//  * Fixed green cast on YUYV camera + clean RGB24 for both sides
//  */

// //#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include <SDL2/SDL.h>
// #include <memory>
// #include <vector>
// #include <chrono>
// #include <atomic>
// #include <string>
// #include <spdlog/spdlog.h>

// #include "../Interfaces/IDisplay.h"
// #include "../SharedStructures/ZeroCopyFrameData.h"
// #include "../SharedStructures/SharedQueue.h"
// #include "../Interfaces/ISystemMetricsAggregator.h"
// #include "../SharedStructures/allModulesStatcs.h"

// struct SDL_Deleter {
//     void operator()(SDL_Window* w)   const { if (w) SDL_DestroyWindow(w); }
//     void operator()(SDL_Renderer* r) const { if (r) SDL_DestroyRenderer(r); }
//     void operator()(SDL_Texture* t)  const { if (t) SDL_DestroyTexture(t); }
// };

// template<typename T>
// inline T clampValue(T val, T minV, T maxV) {
//     return (val < minV) ? minV : (val > maxV) ? maxV : val;
// }

// class SdlDisplayConcrete final : public IDisplay {
// private:
//     std::vector<uint8_t> conversionBuffer_;
//     static constexpr size_t MaxFrameSize = 320 * 240 * 3;

//     std::unique_ptr<SDL_Window,   SDL_Deleter> window_{nullptr};
//     std::unique_ptr<SDL_Renderer, SDL_Deleter> renderer_{nullptr};
//     std::unique_ptr<SDL_Texture,  SDL_Deleter> texOrig_{nullptr};
//     std::unique_ptr<SDL_Texture,  SDL_Deleter> texProc_{nullptr};

//     std::shared_ptr<ZeroCopyFrameData> lastOriginal_;
//     std::shared_ptr<ZeroCopyFrameData> lastProcessed_;

//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> originalQueue_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> processedQueue_;
//     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

//     int displayWidth_ = 640;
//     int displayHeight_ = 240;
//     int frameWidth_ = 0;
//     int frameHeight_ = 0;

//     std::chrono::steady_clock::time_point fpsTimer_;
//     double origFps_ = 0.0;
//     double procFps_ = 0.0;
//     int origCount_ = 0;
//     int procCount_ = 0;

//     std::atomic<bool> running_{true};
//     bool initialized_ = false;

// public:
//     explicit SdlDisplayConcrete(
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> origQ,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> procQ,
//         std::shared_ptr<ISystemMetricsAggregator> agg)
//         : originalQueue_(std::move(origQ)),
//           processedQueue_(std::move(procQ)),
//           metricAggregator_(std::move(agg)),
//           fpsTimer_(std::chrono::steady_clock::now()),
//           conversionBuffer_(MaxFrameSize) {
//         spdlog::info("[SdlDisplayConcrete] v3.3 - YUYV camera fix.");
//     }

//     ~SdlDisplayConcrete() override = default;

//     bool configure(const DisplayConfig&) override { return true; }

//     bool initializeDisplay(int width, int height) override {
//         if (initialized_) return true;

//         if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;

//         window_.reset(SDL_CreateWindow("ERL Display [Original | Processed]",
//             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
//             displayWidth_, displayHeight_, SDL_WINDOW_SHOWN));

//         renderer_.reset(SDL_CreateRenderer(window_.get(), -1,
//             SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));

//         if (!renderer_) return false;

//         texOrig_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
//             SDL_TEXTUREACCESS_STREAMING, width, height));
//         texProc_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
//             SDL_TEXTUREACCESS_STREAMING, width, height));

//         frameWidth_ = width;
//         frameHeight_ = height;
//         initialized_ = true;
//         return true;
//     }

//     void renderAndPollEvents() override {
//         if (!running_.load() || !renderer_) return;

//         SDL_Event e;
//         while (SDL_PollEvent(&e)) {
//             if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
//                 running_ = false;
//                 return;
//             }
//         }

//         if (originalQueue_->size() == 0) {
//             spdlog::warn("[Display] Original queue STARVING");
//         }
//         drainQueue_(originalQueue_, lastOriginal_);
//         drainQueue_(processedQueue_, lastProcessed_);
//         if (processedQueue_->size() == 0) {
//             spdlog::warn("[Display] Processed queue STARVING");
//         }

//         const bool hasOrig = lastOriginal_ && lastOriginal_->isValid();
//         const bool hasProc = lastProcessed_ && lastProcessed_->isValid();

//         if ((hasOrig || hasProc) && frameWidth_ == 0) {
//             int w = hasOrig ? lastOriginal_->width : lastProcessed_->width;
//             int h = hasOrig ? lastOriginal_->height : lastProcessed_->height;
//             initializeDisplay(w, h);
//         }

//         if (hasOrig && texOrig_) {
//             uploadRGB24Texture(texOrig_.get(), lastOriginal_);
//             ++origCount_;
//         }
//         if (hasProc && texProc_) {
//             uploadRGB24Texture(texProc_.get(), lastProcessed_);
//             ++procCount_;
//         }

//         auto now = std::chrono::steady_clock::now();
//         if (std::chrono::duration<double>(now - fpsTimer_).count() >= 1.0) {
//             origFps_ = static_cast<double>(origCount_);
//             procFps_ = static_cast<double>(procCount_);
//             origCount_ = procCount_ = 0;
//             fpsTimer_ = now;
//         }

//         SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
//         SDL_RenderClear(renderer_.get());

//         if (texOrig_ && hasOrig) {
//             SDL_Rect r{0, 0, displayWidth_/2, displayHeight_};
//             SDL_RenderCopy(renderer_.get(), texOrig_.get(), nullptr, &r);
//         }
//         if (texProc_ && hasProc) {
//             SDL_Rect r{displayWidth_/2, 0, displayWidth_/2, displayHeight_};
//             SDL_RenderCopy(renderer_.get(), texProc_.get(), nullptr, &r);
//         }

//         SDL_RenderPresent(renderer_.get());

//         if (metricAggregator_) {
//             DisplayStats stats{};
//             stats.frameId = hasOrig ? lastOriginal_->frameNumber : 0;
//             stats.fps = origFps_;
//             metricAggregator_->mergeDisplay(stats.frameId, stats);
//         }
//     }

//     void updateOriginalFrame(const uint8_t*, int, int) override {}
//     void updateProcessedFrame(const uint8_t*, int, int) override {}
//     void setErrorCallback(std::function<void(const std::string&)>) override {}
//     void closeDisplay() override { running_ = false; }
//     bool is_Running() override { return running_.load(); }

// private:
//    template <typename QueueT>
//    void drainQueue_(QueueT& q, std::shared_ptr<ZeroCopyFrameData>& last) {
//    // void drainQueue_(auto& q, std::shared_ptr<ZeroCopyFrameData>& last) {
//         if (!q) return;
//         std::shared_ptr<ZeroCopyFrameData> f;
//         while (q->try_pop(f)) {
//             if (f && f->isValid()) last = f;
//         }
//     }

//     // FIXED YUYV ? RGB24 conversion (corrected coefficients for typical webcams)
//     void convertYUYVtoRGB24(const uint8_t* yuyv, int w, int h, uint8_t* rgb) {
//         for (int i = 0; i < w * h; i += 2) {
//             uint8_t y0 = yuyv[i*2];
//             uint8_t u  = yuyv[i*2 + 1];
//             uint8_t y1 = yuyv[i*2 + 2];
//             uint8_t v  = yuyv[i*2 + 3];

//             int c0 = y0 - 16;
//             int c1 = y1 - 16;
//             int d = u - 128;
//             int e = v - 128;

//             // Improved coefficients for common YUYV cameras (less green cast)
//             rgb[i*3]   = clampValue((298 * c0 + 409 * e + 128) >> 8, 0, 255);
//             rgb[i*3+1] = clampValue((298 * c0 - 100 * d - 208 * e + 128) >> 8, 0, 255);
//             rgb[i*3+2] = clampValue((298 * c0 + 516 * d + 128) >> 8, 0, 255);

//             rgb[(i+1)*3]   = clampValue((298 * c1 + 409 * e + 128) >> 8, 0, 255);
//             rgb[(i+1)*3+1] = clampValue((298 * c1 - 100 * d - 208 * e + 128) >> 8, 0, 255);
//             rgb[(i+1)*3+2] = clampValue((298 * c1 + 516 * d + 128) >> 8, 0, 255);
//         }
//     }

//     void uploadRGB24Texture(SDL_Texture* tex, const std::shared_ptr<ZeroCopyFrameData>& frame) {
//         if (!tex || !frame || !frame->isValid() || !frame->dataPtr) return;

//         convertYUYVtoRGB24(static_cast<const uint8_t*>(frame->dataPtr),
//                            frame->width, frame->height, conversionBuffer_.data());

//         SDL_UpdateTexture(tex, nullptr, conversionBuffer_.data(), frame->width * 3);
//     }
// };

//==============================================================================================================================


// #pragma once
// /**
//  * SdlDisplayConcrete_new.h - FINAL TARGETED FIX v3.3
//  * Fixed green cast on YUYV camera + clean RGB24 for both sides
//  */

// //#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include <SDL2/SDL.h>
// #include <memory>
// #include <vector>
// #include <chrono>
// #include <atomic>
// #include <string>
// #include <spdlog/spdlog.h>

// #include "../Interfaces/IDisplay.h"
// #include "../SharedStructures/ZeroCopyFrameData.h"
// #include "../SharedStructures/SharedQueue.h"
// #include "../Interfaces/ISystemMetricsAggregator.h"
// #include "../SharedStructures/allModulesStatcs.h"

// struct SDL_Deleter {
//     void operator()(SDL_Window* w)   const { if (w) SDL_DestroyWindow(w); }
//     void operator()(SDL_Renderer* r) const { if (r) SDL_DestroyRenderer(r); }
//     void operator()(SDL_Texture* t)  const { if (t) SDL_DestroyTexture(t); }
// };

// template<typename T>
// inline T clampValue(T val, T minV, T maxV) {
//     return (val < minV) ? minV : (val > maxV) ? maxV : val;
// }

// class SdlDisplayConcrete final : public IDisplay {
// private:
//     std::vector<uint8_t> conversionBuffer_;
//     static constexpr size_t MaxFrameSize = 320 * 240 * 3;

//     std::unique_ptr<SDL_Window,   SDL_Deleter> window_{nullptr};
//     std::unique_ptr<SDL_Renderer, SDL_Deleter> renderer_{nullptr};
//     std::unique_ptr<SDL_Texture,  SDL_Deleter> texOrig_{nullptr};
//     std::unique_ptr<SDL_Texture,  SDL_Deleter> texProc_{nullptr};

//     std::shared_ptr<ZeroCopyFrameData> lastOriginal_;
//     std::shared_ptr<ZeroCopyFrameData> lastProcessed_;

//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> originalQueue_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> processedQueue_;
//     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

//     int displayWidth_ = 640;
//     int displayHeight_ = 240;
//     int frameWidth_ = 0;
//     int frameHeight_ = 0;

//     std::chrono::steady_clock::time_point fpsTimer_;
//     double origFps_ = 0.0;
//     double procFps_ = 0.0;
//     int origCount_ = 0;
//     int procCount_ = 0;
//     // [PERF] Render-tick dedup: drain-to-last keeps last* valid forever, so
//     // the upload path ran on EVERY vsync tick.
//     uint64_t lastUploadedOrig_ = ~uint64_t{0};
//     uint64_t lastUploadedProc_ = ~uint64_t{0};

//     std::atomic<bool> running_{true};
//     bool initialized_ = false;

// public:
//     explicit SdlDisplayConcrete(
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> origQ,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> procQ,
//         std::shared_ptr<ISystemMetricsAggregator> agg)
//         : originalQueue_(std::move(origQ)),
//           processedQueue_(std::move(procQ)),
//           metricAggregator_(std::move(agg)),
//           fpsTimer_(std::chrono::steady_clock::now()),
//           conversionBuffer_(MaxFrameSize) {
//         spdlog::info("[SdlDisplayConcrete] v3.3 - YUYV camera fix.");
//     }

//     ~SdlDisplayConcrete() override = default;

//     bool configure(const DisplayConfig&) override { return true; }

//     bool initializeDisplay(int width, int height) override {
        
//         if (initialized_) return true;
//          // Disable SDL screensaver inhibition via D-Bus
//         // [CRITICAL FIX] Disable SDL screensaver inhibition via D-Bus to prevent assertion crash on Jetson
//         SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

//         if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;

//         window_.reset(SDL_CreateWindow("ERL Display [Original | Processed]",
//             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
//             displayWidth_, displayHeight_, SDL_WINDOW_SHOWN));

//         // [FIX-1] Never hand a null window to SDL_CreateRenderer (segfault).
//         if (!window_) {
//             spdlog::error("[Display] SDL_CreateWindow failed: {}", SDL_GetError());
//             return false;
//         }

//         renderer_.reset(SDL_CreateRenderer(window_.get(), -1,
//             SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));

//         if (!renderer_) return false;

//         texOrig_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
//             SDL_TEXTUREACCESS_STREAMING, width, height));
//         texProc_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
//             SDL_TEXTUREACCESS_STREAMING, width, height));

//         frameWidth_ = width;
//         frameHeight_ = height;
//         initialized_ = true;
//         return true;
//     }

//     void renderAndPollEvents() override {
//         if (!running_.load() || !renderer_) return;

//         SDL_Event e;
//         while (SDL_PollEvent(&e)) {
//             if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
//                 running_ = false;
//                 return;
//             }
//         }

//         if (originalQueue_->size() == 0) {
//             spdlog::debug("[Display] Original queue empty at poll");   // [F29] benign at 60 Hz poll vs ~13 fps supply
//         }
//         drainQueue_(originalQueue_, lastOriginal_);
//         drainQueue_(processedQueue_, lastProcessed_);
//         if (processedQueue_->size() == 0) {
//             spdlog::debug("[Display] Processed queue empty at poll");   // [F29] benign, see above
//         }

//         const bool hasOrig = lastOriginal_ && lastOriginal_->isValid();
//         const bool hasProc = lastProcessed_ && lastProcessed_->isValid();

//         if ((hasOrig || hasProc) && frameWidth_ == 0) {
//             int w = hasOrig ? lastOriginal_->width : lastProcessed_->width;
//             int h = hasOrig ? lastOriginal_->height : lastProcessed_->height;
//             initializeDisplay(w, h);
//         }

//         // [PERF] Convert/upload only when a NEW frame arrived. Previously
//         // this ran every render tick (60 Hz vsync vs ~13 fps supply in the
//         // 2026-07-20 run): ~4-5x redundant YUYV->RGB conversions per frame
//         // (part of F29's CPU saturation), and origFps_/procFps_ counted
//         // render ticks (~60) instead of frame arrivals - so DisplayStats.fps
//         // never measured the display-side frame rate until now.
//         if (hasOrig && texOrig_ && lastOriginal_->frameNumber != lastUploadedOrig_) {
//             uploadRGB24Texture(texOrig_.get(), lastOriginal_);
//             lastUploadedOrig_ = lastOriginal_->frameNumber;
//             ++origCount_;
//         }
//         if (hasProc && texProc_ && lastProcessed_->frameNumber != lastUploadedProc_) {
//             uploadRGB24Texture(texProc_.get(), lastProcessed_);
//             lastUploadedProc_ = lastProcessed_->frameNumber;
//             ++procCount_;
//         }

//         auto now = std::chrono::steady_clock::now();
//         if (std::chrono::duration<double>(now - fpsTimer_).count() >= 1.0) {
//             origFps_ = static_cast<double>(origCount_);
//             procFps_ = static_cast<double>(procCount_);
//             origCount_ = procCount_ = 0;
//             fpsTimer_ = now;
//         }

//         SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
//         SDL_RenderClear(renderer_.get());

//         if (texOrig_ && hasOrig) {
//             SDL_Rect r{0, 0, displayWidth_/2, displayHeight_};
//             SDL_RenderCopy(renderer_.get(), texOrig_.get(), nullptr, &r);
//         }
//         if (texProc_ && hasProc) {
//             SDL_Rect r{displayWidth_/2, 0, displayWidth_/2, displayHeight_};
//             SDL_RenderCopy(renderer_.get(), texProc_.get(), nullptr, &r);
//         }

//         SDL_RenderPresent(renderer_.get());

//         if (metricAggregator_) {
//             DisplayStats stats{};
//             // [P1-LAT] Contract fix: the aggregator normalizes this field as
//             // a steady_clock stamp (disp_ts_sys = now_sys + (ts - now_steady)).
//             // Left default-constructed (steady epoch = boot), every frame's
//             // displayLatency evaluated to ~minus-uptime (-1.5e6 ms observed)
//             // and was clamped to 0 on 7,383/7,383 frames of the 2026-07-20
//             // run. DisplayStats.timestamp is steady_clock per
//             // allModulesStatcs.h.
//             stats.timestamp = std::chrono::steady_clock::now();
//             // [P1-LAT] Report the PROCESSED frame actually presented; the
//             // original-side id diverges after drain-to-last skips and caused
//             // 4,324 mergeDisplay(frame-not-found) misses.
//             stats.frameId = (lastProcessed_ && lastProcessed_->isValid())
//                                 ? lastProcessed_->frameNumber
//                                 : (hasOrig ? lastOriginal_->frameNumber : 0);
//             stats.fps = (procFps_ > 0.0) ? procFps_ : origFps_;
//             metricAggregator_->mergeDisplay(stats.frameId, stats);
//         }
//     }

//     void updateOriginalFrame(const uint8_t*, int, int) override {}
//     void updateProcessedFrame(const uint8_t*, int, int) override {}
//     void setErrorCallback(std::function<void(const std::string&)>) override {}
//     void closeDisplay() override { running_ = false; }
//     bool is_Running() override { return running_.load(); }

// private:
//    template <typename QueueT>
//    void drainQueue_(QueueT& q, std::shared_ptr<ZeroCopyFrameData>& last) {
//    // void drainQueue_(auto& q, std::shared_ptr<ZeroCopyFrameData>& last) {
//         if (!q) return;
//         std::shared_ptr<ZeroCopyFrameData> f;
//         while (q->try_pop(f)) {
//             if (f && f->isValid()) last = f;
//         }
//     }

//     // FIXED YUYV ? RGB24 conversion (corrected coefficients for typical webcams)
//     void convertYUYVtoRGB24(const uint8_t* yuyv, int w, int h, uint8_t* rgb) {
//         for (int i = 0; i < w * h; i += 2) {
//             uint8_t y0 = yuyv[i*2];
//             uint8_t u  = yuyv[i*2 + 1];
//             uint8_t y1 = yuyv[i*2 + 2];
//             uint8_t v  = yuyv[i*2 + 3];

//             int c0 = y0 - 16;
//             int c1 = y1 - 16;
//             int d = u - 128;
//             int e = v - 128;

//             // Improved coefficients for common YUYV cameras (less green cast)
//             rgb[i*3]   = clampValue((298 * c0 + 409 * e + 128) >> 8, 0, 255);
//             rgb[i*3+1] = clampValue((298 * c0 - 100 * d - 208 * e + 128) >> 8, 0, 255);
//             rgb[i*3+2] = clampValue((298 * c0 + 516 * d + 128) >> 8, 0, 255);

//             rgb[(i+1)*3]   = clampValue((298 * c1 + 409 * e + 128) >> 8, 0, 255);
//             rgb[(i+1)*3+1] = clampValue((298 * c1 - 100 * d - 208 * e + 128) >> 8, 0, 255);
//             rgb[(i+1)*3+2] = clampValue((298 * c1 + 516 * d + 128) >> 8, 0, 255);
//         }
//     }

//     void uploadRGB24Texture(SDL_Texture* tex, const std::shared_ptr<ZeroCopyFrameData>& frame) {
//         if (!tex || !frame || !frame->isValid() || !frame->dataPtr) return;

//         // [FIX-2 complete] Guard BOTH sides of the conversion. The buffer was
//         // fixed at 320*240*3: any larger negotiated frame silently corrupted
//         // the heap (write side), and lying width/height metadata could read
//         // past the source (read side).
//         const size_t neededSrc = static_cast<size_t>(frame->width) * frame->height * 2;  // YUYV
//         const size_t neededDst = static_cast<size_t>(frame->width) * frame->height * 3;  // RGB24
//         if (frame->size < neededSrc) {
//             spdlog::warn("[Display] Frame {}: geometry/size mismatch ({}x{} needs {} B, have {}) - skipping",
//                          frame->frameNumber, frame->width, frame->height, neededSrc, frame->size);
//             return;
//         }
//         if (conversionBuffer_.size() < neededDst) {
//             conversionBuffer_.resize(neededDst);
//         }

//         convertYUYVtoRGB24(static_cast<const uint8_t*>(frame->dataPtr),
//                            frame->width, frame->height, conversionBuffer_.data());

//         SDL_UpdateTexture(tex, nullptr, conversionBuffer_.data(), frame->width * 3);
//     }
// // };





//========================================================================
// #pragma once
// /**
//  * SdlDisplayConcrete.h - TARGETED FIX v3.4 (Complete)
//  * Fixed green cast on YUYV camera + D-Bus crash fix + clean RGB24 conversion
//  */

// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include <cstdlib>   // setenv
// #include <SDL2/SDL.h>
// #include <memory>
// #include <vector>
// #include <chrono>
// #include <atomic>
// #include <string>
// #include <spdlog/spdlog.h>

// #include "../Interfaces/IDisplay.h"
// #include "../SharedStructures/ZeroCopyFrameData.h"
// #include "../SharedStructures/SharedQueue.h"
// #include "../Interfaces/ISystemMetricsAggregator.h"
// #include "../SharedStructures/allModulesStatcs.h"

// struct SDL_Deleter {
//     void operator()(SDL_Window* w)   const { if (w) SDL_DestroyWindow(w); }
//     void operator()(SDL_Renderer* r) const { if (r) SDL_DestroyRenderer(r); }
//     void operator()(SDL_Texture* t)  const { if (t) SDL_DestroyTexture(t); }
// };

// template<typename T>
// inline T clampValue(T val, T minV, T maxV) {
//     return (val < minV) ? minV : (val > maxV) ? maxV : val;
// }

// class SdlDisplayConcrete final : public IDisplay {
// private:
//     std::vector<uint8_t> conversionBuffer_;
//     static constexpr size_t MaxFrameSize = 320 * 240 * 3;

//     std::unique_ptr<SDL_Window,   SDL_Deleter> window_{nullptr};
//     std::unique_ptr<SDL_Renderer, SDL_Deleter> renderer_{nullptr};
//     std::unique_ptr<SDL_Texture,  SDL_Deleter> texOrig_{nullptr};
//     std::unique_ptr<SDL_Texture,  SDL_Deleter> texProc_{nullptr};

//     std::shared_ptr<ZeroCopyFrameData> lastOriginal_;
//     std::shared_ptr<ZeroCopyFrameData> lastProcessed_;

//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> originalQueue_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> processedQueue_;
//     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

//     int displayWidth_ = 640;
//     int displayHeight_ = 240;
//     int frameWidth_ = 0;
//     int frameHeight_ = 0;

//     std::chrono::steady_clock::time_point fpsTimer_;
//     double origFps_ = 0.0;
//     double procFps_ = 0.0;
//     int origCount_ = 0;
//     int procCount_ = 0;

//     // Render-tick deduplication markers
//     uint64_t lastUploadedOrig_ = ~uint64_t{0};
//     uint64_t lastUploadedProc_ = ~uint64_t{0};

//     std::atomic<bool> running_{true};
//     bool initialized_ = false;

// public:
//     explicit SdlDisplayConcrete(
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> origQ,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> procQ,
//         std::shared_ptr<ISystemMetricsAggregator> agg)
//         : originalQueue_(std::move(origQ)),
//           processedQueue_(std::move(procQ)),
//           metricAggregator_(std::move(agg)),
//           fpsTimer_(std::chrono::steady_clock::now()),
//           conversionBuffer_(MaxFrameSize) {
//         spdlog::info("[SdlDisplayConcrete] v3.4 - YUYV camera & D-Bus fix initialized.");
//     }

//     ~SdlDisplayConcrete() override = default;

//     bool configure(const DisplayConfig&) override { return true; }

//     bool initializeDisplay(int width, int height) override {
//         if (initialized_) return true;

//         // [CRITICAL D-BUS FIXES FOR SSH / JETSON]
//         // 1. Disable AT-SPI accessibility bridge (Primary cause of path != NULL crash)
//         ::setenv("NO_AT_BRIDGE", "1", 1);
//         // 2. Disable IBus input method probe in SDL
//         ::setenv("XMODIFIERS", "@im=none", 1);
//         // 3. Prevent libdbus from aborting on missing D-Bus session
//         ::setenv("DBUS_FATAL_WARNINGS", "0", 1);
//         // 4. Disable screensaver inhibit D-Bus calls in SDL
//         SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

//         if (SDL_Init(SDL_INIT_VIDEO) < 0) {
//             spdlog::error("[Display] SDL_Init failed: {}", SDL_GetError());
//             return false;
//         }

//         window_.reset(SDL_CreateWindow("ERL Display [Original | Processed]",
//             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
//             displayWidth_, displayHeight_, SDL_WINDOW_SHOWN));

//         if (!window_) {
//             spdlog::error("[Display] SDL_CreateWindow failed: {}", SDL_GetError());
//             return false;
//         }

//         renderer_.reset(SDL_CreateRenderer(window_.get(), -1,
//             SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));

//         if (!renderer_) {
//             spdlog::error("[Display] SDL_CreateRenderer failed: {}", SDL_GetError());
//             return false;
//         }

//         texOrig_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
//             SDL_TEXTUREACCESS_STREAMING, width, height));
//         texProc_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
//             SDL_TEXTUREACCESS_STREAMING, width, height));

//         frameWidth_ = width;
//         frameHeight_ = height;
//         initialized_ = true;
//         return true;
//     }

//     void renderAndPollEvents() override {
//         if (!running_.load() || !renderer_) return;

//         SDL_Event e;
//         while (SDL_PollEvent(&e)) {
//             if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
//                 running_ = false;
//                 return;
//             }
//         }

//         if (originalQueue_->size() == 0) {
//             spdlog::debug("[Display] Original queue empty at poll");
//         }
//         drainQueue_(originalQueue_, lastOriginal_);
//         drainQueue_(processedQueue_, lastProcessed_);
//         if (processedQueue_->size() == 0) {
//             spdlog::debug("[Display] Processed queue empty at poll");
//         }

//         const bool hasOrig = lastOriginal_ && lastOriginal_->isValid();
//         const bool hasProc = lastProcessed_ && lastProcessed_->isValid();

//         if ((hasOrig || hasProc) && frameWidth_ == 0) {
//             int w = hasOrig ? lastOriginal_->width : lastProcessed_->width;
//             int h = hasOrig ? lastOriginal_->height : lastProcessed_->height;
//             initializeDisplay(w, h);
//         }

//         // Convert and upload texture only when a new frame is received
//         if (hasOrig && texOrig_ && lastOriginal_->frameNumber != lastUploadedOrig_) {
//             uploadRGB24Texture(texOrig_.get(), lastOriginal_);
//             lastUploadedOrig_ = lastOriginal_->frameNumber;
//             ++origCount_;
//         }
//         if (hasProc && texProc_ && lastProcessed_->frameNumber != lastUploadedProc_) {
//             uploadRGB24Texture(texProc_.get(), lastProcessed_);
//             lastUploadedProc_ = lastProcessed_->frameNumber;
//             ++procCount_;
//         }

//         auto now = std::chrono::steady_clock::now();
//         if (std::chrono::duration<double>(now - fpsTimer_).count() >= 1.0) {
//             origFps_ = static_cast<double>(origCount_);
//             procFps_ = static_cast<double>(procCount_);
//             origCount_ = procCount_ = 0;
//             fpsTimer_ = now;
//         }

//         SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
//         SDL_RenderClear(renderer_.get());

//         if (texOrig_ && hasOrig) {
//             SDL_Rect r{0, 0, displayWidth_/2, displayHeight_};
//             SDL_RenderCopy(renderer_.get(), texOrig_.get(), nullptr, &r);
//         }
//         if (texProc_ && hasProc) {
//             SDL_Rect r{displayWidth_/2, 0, displayWidth_/2, displayHeight_};
//             SDL_RenderCopy(renderer_.get(), texProc_.get(), nullptr, &r);
//         }

//         SDL_RenderPresent(renderer_.get());

//         if (metricAggregator_) {
//             DisplayStats stats{};
//             stats.timestamp = std::chrono::steady_clock::now();
//             stats.frameId = (lastProcessed_ && lastProcessed_->isValid())
//                                 ? lastProcessed_->frameNumber
//                                 : (hasOrig ? lastOriginal_->frameNumber : 0);
//             stats.fps = (procFps_ > 0.0) ? procFps_ : origFps_;
//             metricAggregator_->mergeDisplay(stats.frameId, stats);
//         }
//     }

//     void updateOriginalFrame(const uint8_t*, int, int) override {}
//     void updateProcessedFrame(const uint8_t*, int, int) override {}
//     void setErrorCallback(std::function<void(const std::string&)>) override {}
//     void closeDisplay() override { running_ = false; }
//     bool is_Running() override { return running_.load(); }

// private:
//     template <typename QueueT>
//     void drainQueue_(QueueT& q, std::shared_ptr<ZeroCopyFrameData>& last) {
//         if (!q) return;
//         std::shared_ptr<ZeroCopyFrameData> f;
//         while (q->try_pop(f)) {
//             if (f && f->isValid()) last = f;
//         }
//     }

//     // Fixed YUYV -> RGB24 conversion
//     void convertYUYVtoRGB24(const uint8_t* yuyv, int w, int h, uint8_t* rgb) {
//         for (int i = 0; i < w * h; i += 2) {
//             uint8_t y0 = yuyv[i*2];
//             uint8_t u  = yuyv[i*2 + 1];
//             uint8_t y1 = yuyv[i*2 + 2];
//             uint8_t v  = yuyv[i*2 + 3];

//             int c0 = y0 - 16;
//             int c1 = y1 - 16;
//             int d = u - 128;
//             int e = v - 128;

//             rgb[i*3]     = clampValue((298 * c0 + 409 * e + 128) >> 8, 0, 255);
//             rgb[i*3+1]   = clampValue((298 * c0 - 100 * d - 208 * e + 128) >> 8, 0, 255);
//             rgb[i*3+2]   = clampValue((298 * c0 + 516 * d + 128) >> 8, 0, 255);

//             rgb[(i+1)*3]   = clampValue((298 * c1 + 409 * e + 128) >> 8, 0, 255);
//             rgb[(i+1)*3+1] = clampValue((298 * c1 - 100 * d - 208 * e + 128) >> 8, 0, 255);
//             rgb[(i+1)*3+2] = clampValue((298 * c1 + 516 * d + 128) >> 8, 0, 255);
//         }
//     }

//     void uploadRGB24Texture(SDL_Texture* tex, const std::shared_ptr<ZeroCopyFrameData>& frame) {
//         if (!tex || !frame || !frame->isValid() || !frame->dataPtr) return;

//         const size_t neededSrc = static_cast<size_t>(frame->width) * frame->height * 2;  // YUYV
//         const size_t neededDst = static_cast<size_t>(frame->width) * frame->height * 3;  // RGB24

//         if (frame->size < neededSrc) {
//             spdlog::warn("[Display] Frame {}: geometry/size mismatch ({}x{} needs {} B, have {}) - skipping",
//                          frame->frameNumber, frame->width, frame->height, neededSrc, frame->size);
//             return;
//         }

//         if (conversionBuffer_.size() < neededDst) {
//             conversionBuffer_.resize(neededDst);
//         }

//         convertYUYVtoRGB24(static_cast<const uint8_t*>(frame->dataPtr),
//                            frame->width, frame->height, conversionBuffer_.data());

//         SDL_UpdateTexture(tex, nullptr, conversionBuffer_.data(), frame->width * 3);
//     }
// };


//===============================================  NWE


#pragma once
/**
 * SdlDisplayConcrete_new.h - FINAL TARGETED FIX v3.3
 * Fixed green cast on YUYV camera + clean RGB24 for both sides
 */

//#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include <cstdlib>   // [DBUS-GUARD] setenv
#include <SDL2/SDL.h>
#include <memory>
#include <vector>
#include <chrono>
#include <atomic>
#include <string>
#include <spdlog/spdlog.h>

#include "../Interfaces/IDisplay.h"
#include "../SharedStructures/ZeroCopyFrameData.h"
#include "../SharedStructures/SharedQueue.h"
#include "../Interfaces/ISystemMetricsAggregator.h"
#include "../SharedStructures/allModulesStatcs.h"

struct SDL_Deleter {
    void operator()(SDL_Window* w)   const { if (w) SDL_DestroyWindow(w); }
    void operator()(SDL_Renderer* r) const { if (r) SDL_DestroyRenderer(r); }
    void operator()(SDL_Texture* t)  const { if (t) SDL_DestroyTexture(t); }
};

template<typename T>
inline T clampValue(T val, T minV, T maxV) {
    return (val < minV) ? minV : (val > maxV) ? maxV : val;
}

class SdlDisplayConcrete final : public IDisplay {
private:
    std::vector<uint8_t> conversionBuffer_;
    static constexpr size_t MaxFrameSize = 320 * 240 * 3;

    std::unique_ptr<SDL_Window,   SDL_Deleter> window_{nullptr};
    std::unique_ptr<SDL_Renderer, SDL_Deleter> renderer_{nullptr};
    std::unique_ptr<SDL_Texture,  SDL_Deleter> texOrig_{nullptr};
    std::unique_ptr<SDL_Texture,  SDL_Deleter> texProc_{nullptr};

    std::shared_ptr<ZeroCopyFrameData> lastOriginal_;
    std::shared_ptr<ZeroCopyFrameData> lastProcessed_;

    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> originalQueue_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> processedQueue_;
    std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

    int displayWidth_ = 640;
    int displayHeight_ = 240;
    int frameWidth_ = 0;
    int frameHeight_ = 0;

    std::chrono::steady_clock::time_point fpsTimer_;
    double origFps_ = 0.0;
    double procFps_ = 0.0;
    int origCount_ = 0;
    int procCount_ = 0;
    // [PERF] Render-tick dedup (drain-to-last keeps last* valid forever).
    uint64_t lastUploadedOrig_ = ~uint64_t{0};
    uint64_t lastUploadedProc_ = ~uint64_t{0};

    std::atomic<bool> running_{true};
    bool initialized_ = false;

public:
    explicit SdlDisplayConcrete(
        std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> origQ,
        std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> procQ,
        std::shared_ptr<ISystemMetricsAggregator> agg)
        : originalQueue_(std::move(origQ)),
          processedQueue_(std::move(procQ)),
          metricAggregator_(std::move(agg)),
          fpsTimer_(std::chrono::steady_clock::now()),
          conversionBuffer_(MaxFrameSize) {
        spdlog::info("[SdlDisplayConcrete] v3.3 - YUYV camera fix.");
    }

    ~SdlDisplayConcrete() override = default;

    bool configure(const DisplayConfig&) override { return true; }

    bool initializeDisplay(int width, int height) override {
        if (initialized_) return true;

        // [DBUS-GUARD] Four guards against the SDL 2.0.8 / Jetson libdbus
        // abort ("path != NULL") over SSH - covers at-spi, IBus, and the
        // screensaver inhibitor (any of the three D-Bus consumers in this
        // process can fire it):
        setenv("NO_AT_BRIDGE", "1", 1);
        setenv("XMODIFIERS", "@im=none", 1);
        setenv("DBUS_FATAL_WARNINGS", "0", 1);
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            // [VIS] v3.3 returned false SILENTLY here - SDL failures were
            // invisible in the log. Now they name themselves.
            spdlog::error("[Display] SDL_Init failed: {}", SDL_GetError());
            return false;
        }

        window_.reset(SDL_CreateWindow("ERL Display [Original | Processed]",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            displayWidth_, displayHeight_, SDL_WINDOW_SHOWN));

        // [FIX-1] Never hand a null window to SDL_CreateRenderer (segfault).
        if (!window_) {
            spdlog::error("[Display] SDL_CreateWindow failed: {}", SDL_GetError());
            return false;
        }

        renderer_.reset(SDL_CreateRenderer(window_.get(), -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));

        if (!renderer_) return false;

        texOrig_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING, width, height));
        texProc_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING, width, height));

        frameWidth_ = width;
        frameHeight_ = height;
        initialized_ = true;
        return true;
    }

    void renderAndPollEvents() override {
        if (!running_.load() || !renderer_) return;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                running_ = false;
                return;
            }
        }

        if (originalQueue_->size() == 0) {
            spdlog::debug("[Display] Original queue empty at poll");   // [F29] benign at 60 Hz poll
        }
        drainQueue_(originalQueue_, lastOriginal_);
        drainQueue_(processedQueue_, lastProcessed_);
        if (processedQueue_->size() == 0) {
            spdlog::debug("[Display] Processed queue empty at poll");  // [F29] benign
        }

        const bool hasOrig = lastOriginal_ && lastOriginal_->isValid();
        const bool hasProc = lastProcessed_ && lastProcessed_->isValid();

        if ((hasOrig || hasProc) && frameWidth_ == 0) {
            int w = hasOrig ? lastOriginal_->width : lastProcessed_->width;
            int h = hasOrig ? lastOriginal_->height : lastProcessed_->height;
            initializeDisplay(w, h);
        }

        // [PERF] Convert/upload only NEW frames: previously ran every vsync
        // tick (60 Hz vs ~13 fps supply): 4-5x redundant conversions/frame,
        // and orig/procFps_ counted render ticks (~60) instead of arrivals.
        if (hasOrig && texOrig_ && lastOriginal_->frameNumber != lastUploadedOrig_) {
            uploadRGB24Texture(texOrig_.get(), lastOriginal_);
            lastUploadedOrig_ = lastOriginal_->frameNumber;
            ++origCount_;
        }
        if (hasProc && texProc_ && lastProcessed_->frameNumber != lastUploadedProc_) {
            uploadRGB24Texture(texProc_.get(), lastProcessed_);
            lastUploadedProc_ = lastProcessed_->frameNumber;
            ++procCount_;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - fpsTimer_).count() >= 1.0) {
            origFps_ = static_cast<double>(origCount_);
            procFps_ = static_cast<double>(procCount_);
            origCount_ = procCount_ = 0;
            fpsTimer_ = now;
        }

        SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(renderer_.get());

        if (texOrig_ && hasOrig) {
            SDL_Rect r{0, 0, displayWidth_/2, displayHeight_};
            SDL_RenderCopy(renderer_.get(), texOrig_.get(), nullptr, &r);
        }
        if (texProc_ && hasProc) {
            SDL_Rect r{displayWidth_/2, 0, displayWidth_/2, displayHeight_};
            SDL_RenderCopy(renderer_.get(), texProc_.get(), nullptr, &r);
        }

        SDL_RenderPresent(renderer_.get());

        if (metricAggregator_) {
            DisplayStats stats{};
            // [P1-LAT] Aggregator contract: steady_clock stamp (was default-
            // constructed -> displayLatency = -uptime, clamped on every frame).
            stats.timestamp = std::chrono::steady_clock::now();
            // [P1-LAT] Report the PROCESSED frame actually presented.
            stats.frameId = (lastProcessed_ && lastProcessed_->isValid())
                                ? lastProcessed_->frameNumber
                                : (hasOrig ? lastOriginal_->frameNumber : 0);
            stats.fps = (procFps_ > 0.0) ? procFps_ : origFps_;
            metricAggregator_->mergeDisplay(stats.frameId, stats);
        }
    }

    void updateOriginalFrame(const uint8_t*, int, int) override {}
    void updateProcessedFrame(const uint8_t*, int, int) override {}
    void setErrorCallback(std::function<void(const std::string&)>) override {}
    void closeDisplay() override { running_ = false; }
    bool is_Running() override { return running_.load(); }

private:
   template <typename QueueT>
   void drainQueue_(QueueT& q, std::shared_ptr<ZeroCopyFrameData>& last) {
   // void drainQueue_(auto& q, std::shared_ptr<ZeroCopyFrameData>& last) {
        if (!q) return;
        std::shared_ptr<ZeroCopyFrameData> f;
        while (q->try_pop(f)) {
            if (f && f->isValid()) last = f;
        }
    }

    // FIXED YUYV ? RGB24 conversion (corrected coefficients for typical webcams)
    void convertYUYVtoRGB24(const uint8_t* yuyv, int w, int h, uint8_t* rgb) {
        for (int i = 0; i < w * h; i += 2) {
            uint8_t y0 = yuyv[i*2];
            uint8_t u  = yuyv[i*2 + 1];
            uint8_t y1 = yuyv[i*2 + 2];
            uint8_t v  = yuyv[i*2 + 3];

            int c0 = y0 - 16;
            int c1 = y1 - 16;
            int d = u - 128;
            int e = v - 128;

            // Improved coefficients for common YUYV cameras (less green cast)
            rgb[i*3]   = clampValue((298 * c0 + 409 * e + 128) >> 8, 0, 255);
            rgb[i*3+1] = clampValue((298 * c0 - 100 * d - 208 * e + 128) >> 8, 0, 255);
            rgb[i*3+2] = clampValue((298 * c0 + 516 * d + 128) >> 8, 0, 255);

            rgb[(i+1)*3]   = clampValue((298 * c1 + 409 * e + 128) >> 8, 0, 255);
            rgb[(i+1)*3+1] = clampValue((298 * c1 - 100 * d - 208 * e + 128) >> 8, 0, 255);
            rgb[(i+1)*3+2] = clampValue((298 * c1 + 516 * d + 128) >> 8, 0, 255);
        }
    }

    void uploadRGB24Texture(SDL_Texture* tex, const std::shared_ptr<ZeroCopyFrameData>& frame) {
        if (!tex || !frame || !frame->isValid() || !frame->dataPtr) return;

        // [FIX-2 complete] Guard BOTH sides of the conversion (heap overflow
        // on the write side, over-read on the source side).
        const size_t neededSrc = static_cast<size_t>(frame->width) * frame->height * 2;  // YUYV
        const size_t neededDst = static_cast<size_t>(frame->width) * frame->height * 3;  // RGB24
        if (frame->size < neededSrc) {
            spdlog::warn("[Display] Frame {}: geometry/size mismatch ({}x{} needs {} B, have {}) - skipping",
                         frame->frameNumber, frame->width, frame->height, neededSrc, frame->size);
            return;
        }
        if (conversionBuffer_.size() < neededDst) {
            conversionBuffer_.resize(neededDst);
        }

        convertYUYVtoRGB24(static_cast<const uint8_t*>(frame->dataPtr),
                           frame->width, frame->height, conversionBuffer_.data());

        SDL_UpdateTexture(tex, nullptr, conversionBuffer_.data(), frame->width * 3);
    }
};