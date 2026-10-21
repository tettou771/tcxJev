#pragma once
// Internal scheduling/encoding adapter. Vision wire policy lives in VisionWire.cpp.
#include "tcxJev.h"
#include <mutex>
#include <limits>

namespace tcx::jev::detail {
struct ImageBudget {
    std::mutex mutex;
    ImageLimits limits;
    size_t snapshots = 0;
    size_t pending = 0;
    bool open = true;
};
struct Snapshot {
    std::shared_ptr<ImageBudget> budget;
    size_t charged = 0;
    int width = 0, height = 0, channels = 0;
    bool sourceTransparent = false; // Preserve Auto PNG even if averaging rounds alpha to 255.
    ImageOptions options;
    std::string reference;
    std::vector<unsigned char> bytes;
    ~Snapshot();
};
struct PendingImages {
    std::shared_ptr<ImageBudget> budget;
    size_t charged = 0;
    ~PendingImages();
};
struct VisionAccess {
    static const auto& budget(const Request& r) { return r.imageBudget_; }
    static const auto& images(const Request& r) { return r.images_; }
    static const auto& error(const Request& r) { return r.imageError_; }
    static bool opted(const Request& r) { return r.imageOptIn_; }
};
inline bool checkedAdd(size_t& total, size_t bytes, size_t limit) {
    // Bound (abi): subtraction prevents overflow and keeps inclusive byte limits.
    if (total > limit || bytes > limit - total) return false;
    total += bytes;
    return true;
}
inline bool checkedMultiply(size_t a, size_t b, size_t& out) {
    // Bound (abi): dimensions and payload products must fit size_t, including wasm32.
    if (b && a > (std::numeric_limits<size_t>::max)() / b) return false;
    out = a * b;
    return true;
}
struct EncodedImage {
    std::string bytes;
    bool png = false;
};
std::string newReference();
std::string validateImageInput(const tc::Pixels&, const ImageOptions&, int& width, int& height, size_t& bytes);
size_t encodedFileLimit();
EncodedImage encodeImage(const Snapshot&, size_t maxBytes);
#ifdef TCX_JEV_TESTING
// Thread-local fault injection, compiled only by the addon test project.
void setCodecAllocationFailureForTest(size_t mallocCall, size_t reallocCall);
size_t codecLiveAllocationsForTest();
bool codecAllocationFailureTriggeredForTest();
#endif
bool hasBox(const tc::Json& questions);
tc::Json boxQuestion(const tc::Json& instructions);
void addManifest(tc::Json&, const Request&);
std::string prepareVision(const Request&, const std::string& model, TransportRequest&);
void packVision(const Request&, tc::Json, const std::vector<EncodedImage>&, TransportRequest&,
                const std::function<std::string()>& boundarySource = {});
bool parseBoxAnswer(const tc::Json&, Answer&, std::string& error);
} // namespace tcx::jev::detail
