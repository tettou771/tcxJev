// Compile separately, before any TrussC/stb declaration (no unity build/PCH).
#if defined(INCLUDE_STB_IMAGE_WRITE_H) || defined(STBIWDEF) || defined(STBIW_ZLIB_COMPRESS) || \
    defined(STBIW_MALLOC) || defined(STBIW_REALLOC) || defined(STBIW_REALLOC_SIZED) || defined(STBIW_FREE)
#error "tcxJev's private encoder cannot inherit stb linkage/compression overrides"
#endif
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {
// stb's C implementation has unchecked growth paths. Throw before they can
// consume a null allocation; this C++ translation unit unwinds to prepareVision.
// Track every allocation intrusively so an interrupted codec also frees buffers
// that stb would normally release only at the end of compression.
struct alignas(std::max_align_t) CodecBlock {
    CodecBlock* previous;
    CodecBlock* next;
};
struct CodecArena;
thread_local CodecArena* codecArena = nullptr;
#ifdef TCX_JEV_TESTING
struct CodecFault {
    size_t mallocCall = 0, reallocCall = 0, live = 0;
    bool triggered = false;
};
thread_local CodecFault codecFault;
bool failCodecAllocation(size_t& countdown) {
    if (!countdown || --countdown) return false;
    codecFault.triggered = true;
    return true;
}
#endif
struct CodecArena {
    CodecBlock* first = nullptr;
    CodecArena* previous = codecArena;
    CodecArena() { codecArena = this; }
    ~CodecArena();
    CodecArena(const CodecArena&) = delete;
    CodecArena& operator=(const CodecArena&) = delete;
};
void codecFree(void* data) noexcept {
    if (!data) return;
    auto block = static_cast<CodecBlock*>(data) - 1;
    if (block->previous) block->previous->next = block->next;
    else codecArena->first = block->next;
    if (block->next) block->next->previous = block->previous;
    std::free(block);
#ifdef TCX_JEV_TESTING
    --codecFault.live;
#endif
}
CodecArena::~CodecArena() {
    while (first) codecFree(first + 1);
    codecArena = previous;
}
void* codecAllocate(void* data, size_t size, bool reallocate) {
    if (size > (std::numeric_limits<size_t>::max)() - sizeof(CodecBlock)) throw std::bad_alloc();
    bool fail = false;
#ifdef TCX_JEV_TESTING
    fail = failCodecAllocation(reallocate ? codecFault.reallocCall : codecFault.mallocCall);
#endif
    auto old = data ? static_cast<CodecBlock*>(data) - 1 : nullptr;
    auto previous = old ? old->previous : nullptr;
    auto next = old ? old->next : codecArena->first;
    // On failure realloc retains the old block, still owned by the arena.
    auto block = static_cast<CodecBlock*>(fail ? nullptr :
        (reallocate ? std::realloc(old, sizeof(CodecBlock) + size) : std::malloc(sizeof(CodecBlock) + size)));
    if (!block) throw std::bad_alloc();
    block->previous = previous;
    block->next = next;
    if (previous) previous->next = block;
    else codecArena->first = block;
    if (next) next->previous = block;
#ifdef TCX_JEV_TESTING
    if (!data) ++codecFault.live;
#endif
    return block + 1;
}
void* codecMalloc(size_t size) { return codecAllocate(nullptr, size, false); }
void* codecRealloc(void* data, size_t size) { return codecAllocate(data, size, true); }
} // namespace
#define STBIW_MALLOC(size) codecMalloc(size)
#define STBIW_REALLOC(data, size) codecRealloc(data, size)
#define STBIW_FREE(data) codecFree(data)
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb/stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "tcxJevVisionInternal.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace tcx::jev::detail {
namespace {
// The private stb defaults stay fixed: flip=0, PNG compression=8, filter=-1,
// force_png_filter=-1, JPEG settings entirely determined by jpegQuality.
// No public encoder entry point can mutate this translation unit's settings.
double linearToSrgb(double x) { return x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - .055; }
double srgbToLinear(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
unsigned char byte(double x) { return static_cast<unsigned char>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); }
struct Sink {
    char* data = nullptr;
    size_t size = 0, capacity = 0;
    size_t limit;
    bool exceeded = false;
    std::exception_ptr failure;
    static void write(void* context, void* data, int size) noexcept {
        auto& sink = *static_cast<Sink*>(context);
        if (sink.exceeded || sink.failure) return;
        // Bound (codec): cap encoded output before appending; no int-to-size_t wrap.
        if (size < 0 || sink.size > sink.limit || size_t(size) > sink.limit - sink.size) {
            sink.exceeded = true;
            return;
        }
        try {
            const size_t required = sink.size + size_t(size);
            if (required > sink.capacity) {
                const size_t grown = sink.capacity > sink.limit / 2 ? sink.limit : sink.capacity * 2;
                const size_t capacity = (std::max)(required, (std::min)(sink.limit, (std::max)(size_t(1024), grown)));
                // JPEG uses stack scratch; its first heap growth is this output
                // buffer, covered by the same allocator as PNG compression.
                sink.data = static_cast<char*>(codecRealloc(sink.data, capacity));
                sink.capacity = capacity;
            }
            if (size) std::memcpy(sink.data + sink.size, data, size_t(size));
            sink.size = required;
        }
        catch (...) { sink.failure = std::current_exception(); }
    }
};
} // namespace

#ifdef TCX_JEV_TESTING
void setCodecAllocationFailureForTest(size_t mallocCall, size_t reallocCall) {
    codecFault.mallocCall = mallocCall;
    codecFault.reallocCall = reallocCall;
    codecFault.triggered = false;
}
size_t codecLiveAllocationsForTest() { return codecFault.live; }
bool codecAllocationFailureTriggeredForTest() { return codecFault.triggered; }
#endif

EncodedImage encodeImage(const Snapshot& image, size_t maxBytes) {
    bool transparent = image.sourceTransparent;
    const bool hasAlpha = image.channels == 2 || image.channels == 4;
    if (hasAlpha) {
        for (size_t i = size_t(image.channels - 1); i < image.bytes.size(); i += size_t(image.channels))
            if (image.bytes[i] != 255) { transparent = true; break; }
    }
    bool png = image.options.encoding == ImageEncoding::PNG ||
        (image.options.encoding == ImageEncoding::Auto && transparent);
    const int channels = png ? 4 : 3;
    size_t count = 0;
    // Bound (abi): canonical buffer/row byte counts must fit size_t before allocation.
    if (!checkedMultiply(size_t(image.width), size_t(image.height), count) ||
        !checkedMultiply(count, size_t(channels), count)) throw std::runtime_error("local validation: canonical image size overflow");
    std::vector<unsigned char> canonical(count);
    const double matte[] = {image.options.jpegBackground.r, image.options.jpegBackground.g, image.options.jpegBackground.b};
    for (int y = 0; y < image.height; ++y) {
        int sourceY = image.options.origin == ImageOrigin::TopLeft ? y : image.height - 1 - y;
        for (int x = 0; x < image.width; ++x) {
            size_t src = (size_t(sourceY) * size_t(image.width) + size_t(x)) * size_t(image.channels);
            size_t dst = (size_t(y) * size_t(image.width) + size_t(x)) * size_t(channels);
            double alpha = hasAlpha ? image.bytes[src + size_t(image.channels - 1)] / 255.0 : 1.0;
            for (int c = 0; c < 3; ++c) {
                double value = image.bytes[src + (image.channels >= 3 ? size_t(c) : 0)] / 255.0;
                if (image.options.alpha == AlphaMode::Premultiplied)
                    value = alpha == 0 ? 0 : (std::min)(1.0, value / alpha);
                if (alpha == 0) value = 0;
                if (image.options.colorSpace == ImageColorSpace::Linear) value = linearToSrgb(value);
                if (!png && alpha != 1)
                    value = linearToSrgb(srgbToLinear(value) * alpha + srgbToLinear(matte[c]) * (1 - alpha));
                canonical[dst + size_t(c)] = byte(value);
            }
            if (png) canonical[dst + 3] = byte(alpha);
        }
    }
    CodecArena arena;
    Sink sink{nullptr, 0, 0, maxBytes, false, {}};
    int ok = png ? stbi_write_png_to_func(Sink::write, &sink, image.width, image.height, channels, canonical.data(), image.width * channels)
                 : stbi_write_jpg_to_func(Sink::write, &sink, image.width, image.height, channels, canonical.data(), image.options.jpegQuality);
    if (sink.failure) std::rethrow_exception(sink.failure);
    if (sink.exceeded) throw std::runtime_error("local validation: asset " + image.reference + " encoded more than " + std::to_string(maxBytes) + " bytes");
    if (!ok || sink.size == 0) throw std::runtime_error("local validation: image encoding failed");
    return {std::string(sink.data, sink.size), png};
}
} // namespace tcx::jev::detail
