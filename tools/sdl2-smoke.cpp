// Dependency smoke test: SDL2 software presentation of a 640x480 RGB565 buffer.
// This contains no triangle rasterization or GPU reference implementation.
#include <SDL.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

static_assert(SDL_MAJOR_VERSION == 2, "SDL2 headers are required");

namespace {
constexpr int width = 640;
constexpr int height = 480;

int fail(const char* operation) {
    std::cerr << operation << ": " << SDL_GetError() << '\n';
    return 1;
}

struct Session {
    ~Session() { SDL_Quit(); }
};
} // namespace

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail("SDL_Init");
    Session session;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("SDL2 dependency check", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    if (!window) return fail("SDL_CreateWindow");
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE), SDL_DestroyRenderer);
    if (!renderer) return fail("SDL_CreateRenderer");
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
        SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGB565,
                          SDL_TEXTUREACCESS_STREAMING, width, height), SDL_DestroyTexture);
    if (!texture) return fail("SDL_CreateTexture RGB565");

    const std::uint16_t colors[] = {0xf800, 0x07e0, 0x001f};
    std::vector<std::uint16_t> pixels(width * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[y * width + x] = colors[x * 3 / width];

    if (SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE) != 0)
        return fail("SDL_SetTextureBlendMode");
    if (SDL_UpdateTexture(texture.get(), nullptr, pixels.data(), width * 2) != 0)
        return fail("SDL_UpdateTexture");
    if (SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr) != 0)
        return fail("SDL_RenderCopy");

    // Compare in the framebuffer's format. SDL's RGB565 -> RGB888 conversion
    // can round expanded channels differently across software blitters.
    std::vector<std::uint16_t> readback(width * height);
    if (SDL_RenderReadPixels(renderer.get(), nullptr, SDL_PIXELFORMAT_RGB565,
                             readback.data(), width * 2) != 0)
        return fail("SDL_RenderReadPixels");
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (readback[y * width + x] != pixels[y * width + x]) {
                std::cerr << "RGB565 readback mismatch at " << x << ',' << y
                          << ": actual=0x" << std::hex << readback[y * width + x]
                          << ", expected RGB565=0x" << pixels[y * width + x] << '\n';
                return 1;
            }
        }
    }
    SDL_RenderPresent(renderer.get());

    SDL_version version;
    SDL_GetVersion(&version);
    if (version.major != 2) return fail("SDL2 runtime major mismatch");
    std::cout << "SDL2 " << unsigned(version.major) << '.' << unsigned(version.minor)
              << '.' << unsigned(version.patch)
              << ": 640x480 RGB565 upload and software readback passed (video="
              << SDL_GetCurrentVideoDriver() << ")\n";
    return 0;
}
