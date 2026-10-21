#include "tcxJevVisionInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tcx::jev {
namespace detail {
Snapshot::~Snapshot() {
    // Free payload before returning its reservation to another registering thread.
    std::vector<unsigned char>().swap(bytes);
    if (charged) {
        std::lock_guard<std::mutex> lock(budget->mutex);
        budget->snapshots -= charged;
    }
}
PendingImages::~PendingImages() {
    if (charged) {
        std::lock_guard<std::mutex> lock(budget->mutex);
        budget->pending -= charged;
    }
}
} // namespace detail

namespace {
// Exact box coverage in integer coordinates, followed by byte rounding.
// Average in the declared source color space. Weight straight-alpha colors
// by coverage to avoid transparent-edge halos; premultiplied input stays so.
// Keep source row order here; the encoder still normalizes origin/color/alpha.
void downscale(const tc::Pixels& source, detail::Snapshot& out) {
    const uint64_t sw = uint64_t(source.getWidth()), sh = uint64_t(source.getHeight());
    const uint64_t dw = uint64_t(out.width), dh = uint64_t(out.height);
    const bool hasAlpha = out.channels == 2 || out.channels == 4;
    const bool straight = hasAlpha && out.options.alpha == AlphaMode::Straight;
    const double area = double(sw) * double(sh);
    auto roundedByte = [](double value) {
        return static_cast<unsigned char>(std::lround(std::clamp(value, 0.0, 255.0)));
    };
    for (uint64_t y = 0; y < dh; ++y) {
        const uint64_t top = y * sh, bottom = (y + 1) * sh;
        for (uint64_t x = 0; x < dw; ++x) {
            const uint64_t left = x * sw, right = (x + 1) * sw;
            double sum[4] = {};
            for (uint64_t sy = top / dh; sy < (bottom + dh - 1) / dh; ++sy) {
                const auto wy = (std::min)(bottom, (sy + 1) * dh) - (std::max)(top, sy * dh);
                for (uint64_t sx = left / dw; sx < (right + dw - 1) / dw; ++sx) {
                    const auto wx = (std::min)(right, (sx + 1) * dw) - (std::max)(left, sx * dw);
                    const double weight = double(wx) * double(wy);
                    const auto* pixel = source.getData() + (size_t(sy) * size_t(sw) + size_t(sx)) * size_t(out.channels);
                    const double alpha = hasAlpha ? pixel[out.channels - 1] : 255;
                    if (alpha != 255) out.sourceTransparent = true;
                    for (int c = 0; c < out.channels; ++c)
                        sum[c] += pixel[c] * weight * (straight && c < out.channels - 1 ? alpha : 1);
                }
            }
            auto* pixel = out.bytes.data() + (size_t(y) * size_t(dw) + size_t(x)) * size_t(out.channels);
            for (int c = 0; c < out.channels; ++c) {
                const double divisor = straight && c < out.channels - 1 ? sum[out.channels - 1] : area;
                pixel[c] = roundedByte(divisor == 0 ? 0 : sum[c] / divisor);
            }
        }
    }
}
} // namespace

void to_json(tc::Json& out, const Asset& asset) { out = asset.ref(); }

tc::Rect BoxResult::toPixels(float width, float height) const {
    return {rect.x * width, rect.y * height, rect.width * width, rect.height * height};
}

Asset Request::addImage(const tc::Image& image, ImageOptions options) {
    return addImage(image.getPixels(), options);
}

Asset Request::addImage(const tc::Pixels& pixels, ImageOptions options) {
    imageOptIn_ = true;
    Asset asset;
    if (!imageError_.empty()) return asset;
    auto fail = [&](const std::string& why) {
        asset.ref_.clear();
        imageError_ = "local validation: " + why;
        return asset;
    };
    if (!imageBudget_) return fail("addImage requires Client::imageRequest()");
    size_t bytes = 0;
    int width = 0, height = 0;
    auto error = detail::validateImageInput(pixels, options, width, height, bytes);
    if (!error.empty()) return fail(error);
    try {
        // A stack-only rollback guard also covers failure to allocate the
        // shared Snapshot control block, before any pixel storage exists.
        struct Reservation {
            detail::ImageBudget& budget;
            size_t bytes = 0;
            ~Reservation() {
                if (bytes) {
                    std::lock_guard<std::mutex> lock(budget.mutex);
                    budget.snapshots -= bytes;
                }
            }
        } reservation{*imageBudget_};
        {
            std::lock_guard<std::mutex> lock(imageBudget_->mutex);
            if (!imageBudget_->open) return fail("image Client is closed");
            const size_t limit = imageBudget_->limits.snapshotBytes;
            // Bound (retention): reserve resized snapshot payload before allocation/copy/resampling.
            if (!detail::checkedAdd(imageBudget_->snapshots, bytes, limit)) {
                return fail("snapshot budget exhausted: requested " + std::to_string(bytes) +
                    "; retained " + std::to_string(imageBudget_->snapshots) +
                    "; limit " + std::to_string(limit));
            }
            reservation.bytes = bytes;
        }
        auto snapshot = std::make_shared<detail::Snapshot>();
        snapshot->budget = imageBudget_;
        snapshot->charged = reservation.bytes;
        reservation.bytes = 0; // Snapshot now owns rollback/release, exactly once.
        snapshot->width = width;
        snapshot->height = height;
        snapshot->channels = pixels.getChannels();
        snapshot->options = options;
        snapshot->reference = detail::newReference();
        if (width == pixels.getWidth() && height == pixels.getHeight()) {
            snapshot->bytes.assign(pixels.getData(), pixels.getData() + bytes);
        } else {
            snapshot->bytes.resize(bytes);
            downscale(pixels, *snapshot);
        }
        asset.ref_ = snapshot->reference;
        images_.push_back(std::move(snapshot));
        return asset;
    } catch (...) {
        return fail("image snapshot allocation failed");
    }
}

Request& Request::box(const std::string& name, const tc::Json& instructions) {
    return question(name, detail::boxQuestion(instructions));
}
} // namespace tcx::jev
