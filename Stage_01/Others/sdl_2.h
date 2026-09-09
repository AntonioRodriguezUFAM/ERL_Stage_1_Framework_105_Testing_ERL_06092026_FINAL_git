
//sdl_2.h

#include <SDL2/SDL.h>

/**
 * @brief Initialize SDL2 with a window of the given width and height.
 * @param width The width of the window.
 * @param height The height of the window.
 * @return True if SDL2 initialized successfully; false otherwise.


SDL Methods:

The initSDL function initializes SDL, creates a window, renderer, and textures. It stores the display dimensions and initializes processedRGB_.
The closeSDL function destroys SDL resources.
The yuyvToRGB function efficiently converts YUYV frames to RGB for display.
 */


// --------------------------------------------------
// Capture Loop: Convert Original + Render Both
// --------------------------------------------------

 // SDL methods
    bool initSDL(int width, int height);
    void closeSDL();
    void yuyvToRGB(const uint8_t* yuyv, uint8_t* rgb, int width, int height);


// SDL init
    bool sdlOk = initSDL(defaultConfig.width, defaultConfig.height);
    if (!sdlOk) {
        if (errorCallback_) {
            errorCallback_("[SystemCaptureFactory] SDL init failed; no display.");
        }
    }
class SDL_2 {
public:
void SystemCaptureFactory::setGlobalErrorCallback(std::function<void(const std::string&)> callback) {
    errorCallback_ = std::move(callback);
}

// SDL methods
bool SystemCaptureFactory::initSDL(int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        if (errorCallback_) {
            errorCallback_("[SystemCaptureFactory] SDL init error: " + std::string(SDL_GetError()));
        }
        return false;
    }

    sdlWindow_ = SDL_CreateWindow("ERL - Camera Display",
                                  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  width, height,
                                  SDL_WINDOW_SHOWN);
    if (!sdlWindow_) {
        if (errorCallback_) {
            errorCallback_("[SystemCaptureFactory] Window creation error: " + std::string(SDL_GetError()));
        }
        return false;
    }

    sdlRenderer_ = SDL_CreateRenderer(sdlWindow_, -1,
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdlRenderer_) {
        if (errorCallback_) {
            errorCallback_("[SystemCaptureFactory] Renderer creation error: " + std::string(SDL_GetError()));
        }
        return false;
    }

    texOriginal_ = SDL_CreateTexture(sdlRenderer_,
                                     SDL_PIXELFORMAT_RGB24,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     width, height);
    texProcessed_ = SDL_CreateTexture(sdlRenderer_,
                                      SDL_PIXELFORMAT_RGB24,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      width, height);
    if (!texOriginal_ || !texProcessed_) {
        if (errorCallback_) {
            errorCallback_("[SystemCaptureFactory] Texture creation error: " + std::string(SDL_GetError()));
        }
        return false;
    }

    displayWidth_  = width;
    displayHeight_ = height;
    processedRGB_.resize(width * height * 3, 0);

    return true;
}

void SystemCaptureFactory::closeSDL() {
    if (texOriginal_) {
        SDL_DestroyTexture(texOriginal_);
        texOriginal_ = nullptr;
    }
    if (texProcessed_) {
        SDL_DestroyTexture(texProcessed_);
        texProcessed_ = nullptr;
    }
    if (sdlRenderer_) {
        SDL_DestroyRenderer(sdlRenderer_);
        sdlRenderer_ = nullptr;
    }
    if (sdlWindow_) {
        SDL_DestroyWindow(sdlWindow_);
        sdlWindow_ = nullptr;
    }
    SDL_Quit();
}

// --------------------------------------------------
void SystemCaptureFactory::yuyvToRGB(const uint8_t* yuyv, uint8_t* rgb, int width, int height) {
    // A naive YUYV -> RGB24 conversion. For better performance, consider SIMD or GPU approaches.
    auto clamp = [](int v) { return (v < 0) ? 0 : (v > 255 ? 255 : v); };
    int size = width * height * 2; // YUYV is 2 bytes/pixel

    for (int i = 0, j = 0; i < size; i += 4, j += 6) {
        uint8_t Y0 = yuyv[i+0];
        uint8_t U  = yuyv[i+1];
        uint8_t Y1 = yuyv[i+2];
        uint8_t V  = yuyv[i+3];

        int C1 = Y0 - 16;
        int D  = U  - 128;
        int E  = V  - 128;

        int R1 = clamp(( 298 * C1           + 409 * E + 128) >> 8);
        int G1 = clamp(( 298 * C1 - 100 * D - 208 * E + 128) >> 8);
        int B1 = clamp(( 298 * C1 + 516 * D           + 128) >> 8);

        rgb[j+0] = (uint8_t)R1;
        rgb[j+1] = (uint8_t)G1;
        rgb[j+2] = (uint8_t)B1;

        int C2 = Y1 - 16;
        int R2 = clamp(( 298 * C2           + 409 * E + 128) >> 8);
        int G2 = clamp(( 298 * C2 - 100 * D - 208 * E + 128) >> 8);
        int B2 = clamp(( 298 * C2 + 516 * D           + 128) >> 8);

        rgb[j+3] = (uint8_t)R2;
        rgb[j+4] = (uint8_t)G2;
        rgb[j+5] = (uint8_t)B2;
    }
}


private:
    // Error callback
    std::function<void(const std::string&)> errorCallback_;

    // SDL members
    SDL_Window*   sdlWindow_     = nullptr;
    SDL_Renderer* sdlRenderer_   = nullptr;
    SDL_Texture*  texOriginal_   = nullptr;
    SDL_Texture*  texProcessed_  = nullptr;
    int displayWidth_            = 640;
    int displayHeight_           = 480;

    // Buffer for processed data
    std::vector<uint8_t> processedRGB_;
    std::mutex           processedMutex_;
};