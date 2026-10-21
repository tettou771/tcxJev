// All Dreamer-ready wire policy: identities, manifest, reference traversal,
// multipart framing, upstream limits, opt-in header, Box schema and parsing.
#include "tcxJevVisionInternal.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <random>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace tcx::jev::detail {
namespace {
constexpr size_t MiB = 1024 * 1024;
constexpr size_t fileLimit = 16 * MiB;
constexpr size_t aggregateLimit = 32 * MiB;
constexpr const char* prefix = "tcxjev-asset-";
constexpr size_t refLength = 13 + 32;

[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error("local validation: " + message);
}
void addBounded(size_t& total, size_t bytes, size_t limit, const char* label) {
    if (!checkedAdd(total, bytes, limit)) invalid(std::string(label) + " exceeds " + std::to_string(limit) + " bytes");
}
bool unit(double v) { return std::isfinite(v) && v >= 0 && v <= 1; }
std::string partName(size_t i) { return "image_" + std::to_string(i); }

// Validate before dump(): strict UTF-8 is checked by dump(), including keys.
void validateJson(const tc::Json& j, size_t containers = 0) {
    if (j.is_object() || j.is_array()) {
        // Bound (API): at most 64 nested object/array levels per content field.
        if (containers >= 64) invalid("JSON nesting exceeds 64 levels");
        for (const auto& child : j) validateJson(child, containers + 1);
    } else if (j.is_number_float() && !std::isfinite(j.get<double>())) {
        // Bound (API): JSON numbers must be finite; dumping NaN as null loses input.
        invalid("JSON contains a non-finite number");
    } else if (j.is_binary() || j.is_discarded()) {
        invalid("content must be JSON text, not binary/discarded JSON");
    }
}
struct References {
    const std::vector<std::shared_ptr<const Snapshot>>& images;
    std::map<std::string, size_t> indices;
    std::vector<size_t> uses;
    size_t removedBytes = 0;
    explicit References(const Request& r) : images(VisionAccess::images(r)), uses(images.size(), 0) {
        size_t referenceBytes = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            const auto& ref = images[i]->reference;
            // Bound (API): references are nonblank, <=1024 UTF-8 bytes and <=65536 combined.
            if (ref.empty() || ref.size() > 1024 || ref.find_first_not_of(" \r\n\t") == std::string::npos)
                invalid("invalid asset reference");
            addBounded(referenceBytes, ref.size(), 65536, "combined reference bytes");
            indices.emplace(ref, i);
        }
    }
    void occurrence(size_t i) {
        // Bound (abi): repeated occurrences and stripped reference bytes cannot wrap.
        if (!checkedAdd(uses[i], 1, (std::numeric_limits<size_t>::max)()) ||
            !checkedAdd(removedBytes, images[i]->reference.size(), (std::numeric_limits<size_t>::max)()))
            invalid("reference count overflow");
    }
    void scan(const tc::Json& j, const std::string& path) {
        if (j.is_string()) {
            const auto& s = j.get_ref<const std::string&>();
            auto whole = indices.find(s);
            if (whole != indices.end()) { occurrence(whole->second); return; }
            size_t pos = 0;
            while ((pos = s.find(prefix, pos)) != std::string::npos) {
                auto token = s.substr(pos, refLength);
                auto it = indices.find(token);
                if (it == indices.end()) {
                    bool wellFormed = token.size() == refLength &&
                        token.find_first_not_of("0123456789abcdef", 13) == std::string::npos;
                    invalid("unattached asset " + (wellFormed ? token : std::string("<malformed>")) + " at " + path);
                }
                occurrence(it->second);
                pos += token.size(); // Same nonoverlapping replacement walk as expanded accounting.
            }
        } else if (j.is_array()) {
            for (size_t i = 0; i < j.size(); ++i) scan(j[i], path + "/" + std::to_string(i));
        } else if (j.is_object()) {
            for (const auto& item : j.items()) {
                std::string key;
                for (char c : item.key()) key += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
                scan(item.value(), path + "/" + key);
            }
        }
    }
    void content(const Request& r) {
        validateJson(r.getState());
        scan(r.getState(), "/state");
        for (const auto& q : r.getQuestions().items()) {
            for (const char* field : {"instructions", "criteria"}) {
                auto it = q.value().find(field);
                if (it != q.value().end()) {
                    validateJson(*it);
                    scan(*it, "/questions/" + q.key() + "/" + field);
                }
            }
        }
        for (size_t i = 0; i < uses.size(); ++i)
            if (!uses[i]) invalid("unused asset " + images[i]->reference);
    }
};
} // namespace

std::string newReference() {
    // Fixed width and a process nonce prevent substring collisions between registrations.
    static const uint64_t nonce = [] {
        std::random_device random;
        return (uint64_t(random()) << 32) ^ uint64_t(random());
    }();
    static std::atomic<uint64_t> serial{0};
    uint64_t n = serial.fetch_add(1, std::memory_order_relaxed);
    // Bound (identity): do not reuse an identity if the registration counter ever wraps.
    if (n == (std::numeric_limits<uint64_t>::max)()) std::terminate();
    std::ostringstream out;
    out.imbue(std::locale::classic()); // the app's global locale may group digits
    out << prefix << std::hex << std::setfill('0') << std::setw(16) << nonce << std::setw(16) << n;
    return out.str();
}

std::string validateImageInput(const tc::Pixels& p, const ImageOptions& o, int& width, int& height, size_t& bytes) {
    // Bound (input): accept allocated U8 pixels with one to four supported channels.
    if (!p.isAllocated() || !p.getData() || p.getFormat() != tc::PixelFormat::U8 ||
        p.getChannels() < 1 || p.getChannels() > 4) return "image must contain U8 gray, gray-alpha, RGB or RGBA pixels";
    width = p.getWidth();
    height = p.getHeight();
    size_t area = 0;
    // Bound (input/abi): validate source storage before any resampling reads.
    if (width <= 0 || height <= 0 ||
        !checkedMultiply(size_t(width), size_t(height), area) ||
        !checkedMultiply(area, size_t(p.getChannels()), bytes) || bytes > p.getTotalBytes())
        return "image source byte count out of range";
    // Bound (options): zero disables resizing; negative bounds are invalid.
    if (o.maxSide < 0) return "maxSide must be nonnegative";
    const int longest = (std::max)(width, height);
    if (o.maxSide > 0 && longest > o.maxSide) {
        // Nearest integer, with exact halves rounded up. uint64_t prevents
        // overflow for all positive int dimensions; neither side can grow.
        auto scaled = [&](int side) {
            return (std::max)(1, int((uint64_t(side) * uint64_t(o.maxSide) + uint64_t(longest / 2)) / uint64_t(longest)));
        };
        width = scaled(width);
        height = scaled(height);
    }
    // Bound (API): check inclusive dimensions/area AFTER the explicitly requested resize.
    if (width < 280 || width > 1568 || height < 280 || height > 1568)
        return "image dimensions must be 280..1568 after optional maxSide downscale";
    if (!checkedMultiply(size_t(width), size_t(height), area) || area < 78400 || area > 2458624 ||
        !checkedMultiply(area, size_t(p.getChannels()), bytes)) return "image area or byte count out of range";
    // Bound (codec): stb JPEG quality is explicitly 1..100, with no implicit fallback.
    if (o.jpegQuality < 1 || o.jpegQuality > 100) return "JPEG quality must be 1..100";
    if ((o.encoding != ImageEncoding::Auto && o.encoding != ImageEncoding::PNG && o.encoding != ImageEncoding::JPEG) ||
        (o.alpha != AlphaMode::Straight && o.alpha != AlphaMode::Premultiplied) ||
        (o.colorSpace != ImageColorSpace::SRGB && o.colorSpace != ImageColorSpace::Linear) ||
        (o.origin != ImageOrigin::TopLeft && o.origin != ImageOrigin::BottomLeft)) return "invalid image options";
    // Bound (color): a JPEG matte is an opaque, finite sRGB color in [0,1].
    if (!unit(o.jpegBackground.r) || !unit(o.jpegBackground.g) || !unit(o.jpegBackground.b) || o.jpegBackground.a != 1)
        return "JPEG background must be opaque sRGB in [0,1]";
    return {};
}
size_t encodedFileLimit() { return fileLimit; }
bool hasBox(const tc::Json& questions) {
    for (const auto& q : questions)
        if (q.is_object() && q.value("type", tc::Json()) == "box") return true;
    return false;
}
tc::Json boxQuestion(const tc::Json& instructions) { return {{"type", "box"}, {"instructions", instructions}}; }
void addManifest(tc::Json& body, const Request& r) {
    const auto& images = VisionAccess::images(r);
    if (images.empty()) return;
    auto& assets = body["assets"] = tc::Json::object();
    for (size_t i = 0; i < images.size(); ++i)
        assets[images[i]->reference] = {{"type", "image"}, {"part", partName(i)}};
}

// Kept callable internally so tests can exercise byte limits with synthetic
// encoded files, without allocating huge images or weakening production limits.
void packVision(const Request& r, tc::Json body, const std::vector<EncodedImage>& files,
                TransportRequest& http, const std::function<std::string()>& boundarySource) {
    const auto& images = VisionAccess::images(r);
    if (!VisionAccess::error(r).empty()) throw std::runtime_error(VisionAccess::error(r));
    bool box = hasBox(r.getQuestions());
    if (box) {
        if (images.empty()) invalid("Box requires an attached, referenced image");
        const auto model = body.at("model").get<std::string>();
        // Bound (model): known Jev 1.13 IDs/alias cannot answer Box questions.
        if (model == "jev-latest" || model == "jev-1.13" || model.compare(0, 9, "jev-1.13.") == 0)
            invalid("Box requires Jev 1.14; choose jev-1.14.0");
        for (const auto& q : r.getQuestions())
            if (q.is_object() && q.value("type", tc::Json()) == "box" && q.contains("criteria"))
                invalid("Box takes no criteria");
    }
    References refs(r);
    refs.content(r);
    if (files.size() != images.size()) invalid("internal image part count mismatch");
    // Validate the entire logical JSON too, including additional raw question fields.
    // Content depth applies separately to state/instructions/criteria above.
    for (const auto& q : r.getQuestions()) {
        if (!q.is_object()) invalid("question must be an object");
        for (const auto& item : q.items()) if (item.key() != "instructions" && item.key() != "criteria") validateJson(item.value());
    }
    body.erase("assets");
    const std::string base = body.dump(); // Strict UTF-8; no replacement policy for opt-in requests.
    size_t textBytes = base.size() - refs.removedBytes;
    size_t encodedDistinct = 0, decodedDistinct = 0;
    size_t encodedExpanded = textBytes, decodedExpanded = textBytes;
    for (size_t i = 0; i < files.size(); ++i) {
        // Bound (API): each encoded PNG/JPEG file is at most 16 MiB.
        if (files[i].bytes.size() > fileLimit) invalid("asset " + images[i]->reference + " encoded " +
            std::to_string(files[i].bytes.size()) + " bytes; limit " + std::to_string(fileLimit));
        // Bound (API): distinct encoded files and each reference-expanded request fit 32 MiB.
        addBounded(encodedDistinct, files[i].bytes.size(), aggregateLimit, "distinct encoded images");
        size_t decoded = 0, expanded = 0;
        if (!checkedMultiply(size_t(images[i]->width), size_t(images[i]->height), decoded) ||
            !checkedMultiply(decoded, 4, decoded)) invalid("decoded image size overflow");
        // Bound (addon guard): decoded RGBA distinct/expanded bytes conservatively fit 32 MiB.
        addBounded(decodedDistinct, decoded, aggregateLimit, "addon decoded distinct image guard");
        if (!checkedMultiply(files[i].bytes.size(), refs.uses[i], expanded)) invalid("expanded image size overflow");
        addBounded(encodedExpanded, expanded, aggregateLimit, "expanded encoded content");
        if (!checkedMultiply(decoded, refs.uses[i], expanded)) invalid("expanded decoded size overflow");
        addBounded(decodedExpanded, expanded, aggregateLimit, "addon decoded expanded guard");
    }
    // Bound (API): text/structure alone also participates in the expanded budgets.
    if (encodedExpanded > aggregateLimit || decodedExpanded > aggregateLimit) invalid("expanded content exceeds 33554432 bytes");
    addManifest(body, r);
    const auto json = body.dump();
    if (images.empty()) { http.body = json; return; }
    std::string boundary;
    for (;;) {
        boundary = boundarySource ? boundarySource() : "tcxjev-boundary-" + newReference();
        // Bound (MIME): generated boundary is a nonempty safe token of at most 70 bytes.
        if (boundary.empty() || boundary.size() > 70 || boundary.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
            invalid("invalid multipart boundary");
        if (json.find(boundary) != std::string::npos) continue;
        bool collision = false;
        for (const auto& file : files) collision |= file.bytes.find(boundary) != std::string::npos;
        if (!collision) break;
    }
    std::string packed;
    auto append = [&](const std::string& s) {
        size_t total = packed.size();
        // Bound (API): measure every actual body byte, including CRLF and final delimiter.
        addBounded(total, s.size(), aggregateLimit, "multipart HTTP body");
        packed.append(s.data(), s.size());
    };
    append("--" + boundary + "\r\nContent-Disposition: form-data; name=\"request\"\r\nContent-Type: application/json\r\n\r\n");
    append(json);
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string name = partName(i);
        append("\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + name +
            (files[i].png ? ".png\"\r\nContent-Type: image/png\r\n\r\n" : ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n"));
        append(files[i].bytes);
    }
    append("\r\n--" + boundary + "--\r\n");
    http.body = std::move(packed);
    http.contentType = "multipart/form-data; boundary=" + boundary;
    http.headers["TypeSafe-Danger-Zone-Images"] = "true";
}

std::string prepareVision(const Request& r, const std::string& model, TransportRequest& http) {
    try {
        if (!VisionAccess::error(r).empty()) return VisionAccess::error(r);
        // Reject bad references and JSON before encoding, using the same traversal as packing.
        References refs(r);
        refs.content(r);
        auto logical = r.toJson(model);
        (void)logical.dump();
        std::vector<EncodedImage> files;
        for (const auto& image : VisionAccess::images(r)) files.push_back(encodeImage(*image, fileLimit));
        packVision(r, std::move(logical), files, http, {});
        return {};
    } catch (const std::exception& e) {
        // Never echo JSON exception diagnostics (they can contain private content).
        if (dynamic_cast<const tc::Json::exception*>(&e)) return "local validation: invalid JSON/UTF-8";
        return std::string(e.what()).compare(0, 18, "local validation: ") == 0 ? e.what() : "local validation: image preparation failed";
    } catch (...) { return "local validation: image preparation failed"; }
}

bool parseBoxAnswer(const tc::Json& a, Answer& answer, std::string& error) {
    if (!a.is_object() || a.value("type", tc::Json()) != "box") return true;
    answer.type = QuestionType::Box;
    answer.confidence = 0; // Box confidence lives only in the optional BoxResult.
    auto malformed = [&] { error = "invalid Box answer: expected finite ordered edges in [0,1] and confidence null or in [0,1]"; return false; };
    auto box = a.find("box");
    if (box == a.end() || !box->is_object()) return malformed();
    double edges[4];
    const char* names[] = {"left", "top", "right", "bottom"};
    for (int i = 0; i < 4; ++i) {
        auto edge = box->find(names[i]);
        // Bound (wire): normalized edges must be finite and in the inclusive unit interval.
        if (edge == box->end() || !edge->is_number() || !unit(edges[i] = edge->get<double>())) return malformed();
    }
    if (edges[0] >= edges[2] || edges[1] >= edges[3]) return malformed();
    BoxResult result;
    result.rect = {float(edges[0]), float(edges[1]), float(edges[2] - edges[0]), float(edges[3] - edges[1])};
    auto confidence = a.find("confidence");
    if (confidence != a.end() && !confidence->is_null()) {
        // Bound (wire): preserve numeric zero; missing/null confidence remains absent.
        if (!confidence->is_number() || !unit(confidence->get<double>())) return malformed();
        result.confidence = confidence->get<double>();
    }
    answer.box = result;
    return true;
}
} // namespace tcx::jev::detail
