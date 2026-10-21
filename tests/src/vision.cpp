#include <tcxJev.h>
#include <tcxJevVisionInternal.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <locale>
#include <mutex>
#include <thread>

using namespace tcx::jev;
using tc::Json;
using Check = void (*)(const std::string&, bool);
void setCoreImageFlip(int);
std::string corePng(const tc::Pixels&);
namespace {
constexpr size_t MiB = 1024 * 1024;
constexpr size_t rawBytes = 280 * 280 * 4;
TransportResponse success() { return {200, R"({"answers":{}})", {}}; }
tc::Pixels pixels(int channels = 4, int width = 280, int height = 280) {
    tc::Pixels p;
    p.allocate(width, height, channels);
    std::memset(p.getData(), 255, p.getTotalBytes());
    return p;
}
Request picture(Client& c, const tc::Pixels& p, ImageOptions options = {}) {
    auto r = c.imageRequest();
    auto a = r.addImage(p, options);
    r.model("jev-1.14.0").state(Json{{"photo", a}}).noul("visible", "Is there a mug in `photo`?");
    return r;
}
struct Capture {
    TransportRequest http;
    ResponseEventArgs event;
    int calls = 0, events = 0;
    tc::EventListener listener;
    void install(Client& c) {
        c.setTransport([this](const TransportRequest& h) { http = h; ++calls; return success(); }).setAsync(false);
        listener = c.responseEvent.listen([this](ResponseEventArgs& e) { event = e; ++events; });
    }
};
struct Part { std::string headers, bytes; };
// Independent multipart reader: no producer helpers, binary-safe delimiters.
std::vector<Part> readMultipart(const TransportRequest& h) {
    const std::string type = "multipart/form-data; boundary=";
    if (h.contentType.compare(0, type.size(), type)) return {};
    const auto boundary = h.contentType.substr(type.size());
    const auto delimiter = "--" + boundary;
    std::vector<Part> parts;
    size_t pos = 0;
    for (;;) {
        if (h.body.compare(pos, delimiter.size(), delimiter)) return {};
        pos += delimiter.size();
        if (h.body.compare(pos, 4, "--\r\n") == 0)
            return pos + 4 == h.body.size() ? parts : std::vector<Part>{};
        if (h.body.compare(pos, 2, "\r\n")) return {};
        pos += 2;
        auto split = h.body.find("\r\n\r\n", pos);
        if (split == std::string::npos) return {};
        auto end = h.body.find("\r\n" + delimiter, split + 4);
        if (end == std::string::npos) return {};
        parts.push_back({h.body.substr(pos, split - pos), h.body.substr(split + 4, end - split - 4)});
        pos = end + 2;
    }
}
std::vector<unsigned char> decode(const std::string& data, int& w, int& h) {
    int channels = 0;
    auto p = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()), int(data.size()), &w, &h, &channels, 4);
    if (!p) return {};
    std::vector<unsigned char> result(p, p + size_t(w) * size_t(h) * 4);
    stbi_image_free(p);
    return result;
}
std::pair<size_t, size_t> charges(const Request& r) {
    auto b = detail::VisionAccess::budget(r);
    std::lock_guard<std::mutex> lock(b->mutex);
    return {b->snapshots, b->pending};
}
bool local(const Capture& cap) {
    return cap.calls == 0 && cap.events == 1 && !cap.event.ok && !cap.event.cancelled &&
        cap.event.statusCode == 0 && cap.event.raw.is_null() && cap.event.error.find("local validation:") == 0;
}
void groupedLocale(Check check) {
    struct GroupedNumbers : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
    };
    const auto previous = std::locale();
    {
        // Restore even if request preparation or a test assertion throws.
        struct RestoreLocale {
            std::locale previous;
            ~RestoreLocale() { std::locale::global(previous); }
        } restore{previous};
        std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
        Client c;
        Capture cap; cap.install(c);
        auto r = c.imageRequest();
        auto asset = r.addImage(pixels());
        const auto& ref = asset.ref();
        check("grouped global locale keeps reference exactly 45 ASCII bytes",
            ref.size() == 45 && ref.compare(0, 13, "tcxjev-asset-") == 0 &&
            ref.find_first_not_of("0123456789abcdef", 13) == std::string::npos);
        r.state(Json{{"photo", asset}, {"number", 1234.5}}).noul("q", "visible?");
        c.setTransport([&](const TransportRequest& h) {
            cap.http = h; ++cap.calls;
            return TransportResponse{200, R"({"answers":{"s":{"type":"score","score":1.25,"legend":{"0":"low","-1":"negative","2":"high","1.000":"grouped"," 3":"space","+4":"plus","2147483648":"overflow","5x":"suffix","":"empty"}}}})", {}};
        });
        const auto id = c.request(r);
        const std::string type = "multipart/form-data; boundary=";
        const auto boundary = cap.http.contentType.compare(0, type.size(), type) == 0
            ? cap.http.contentType.substr(type.size()) : std::string{};
        check("grouped global locale boundary passes MIME token validation",
            !boundary.empty() && boundary.size() <= 70 &&
            boundary.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos &&
            readMultipart(cap.http).size() == 2);
        check("grouped global locale image completes through injected transport",
            id && cap.calls == 1 && cap.events == 1 && cap.event.ok && c.getPendingCount() == 0 && charges(r).second == 0);
        const auto parts = readMultipart(cap.http);
        check("grouped global locale preserves JSON decimal wire bytes",
            parts.size() == 2 && parts[0].bytes.find("\"number\":1234.5") != std::string::npos &&
            cap.event.answers.count("s") && cap.event.answers.at("s").score == 1.25);
        const auto answer = cap.event.answers.find("s");
        check("legend keys accept only complete ASCII integers in int range",
            answer != cap.event.answers.end() && answer->second.legend == std::map<int, std::string>{{-1, "negative"}, {0, "low"}, {2, "high"}});
        // A second request exercises the fixed-width inline reference scanner.
        r.state("Look at " + ref);
        c.request(r);
        check("grouped global locale inline reference completes through injected transport",
            cap.calls == 2 && cap.events == 2 && cap.event.ok && c.getPendingCount() == 0);
    }
    check("grouped global locale regression restores previous locale", std::locale() == previous);
}
void downscaling(Check check) {
    Client c;
    Capture cap; cap.install(c);
    ImageOptions options; options.maxSide = 1568; options.encoding = ImageEncoding::PNG;
    auto hd = pixels(4, 1920, 1080);
    auto resized = picture(c, hd, options);
    const auto& snapshots = detail::VisionAccess::images(resized);
    c.request(resized);
    auto parts = readMultipart(cap.http);
    int w = 0, h = 0;
    auto decoded = parts.size() == 2 ? decode(parts[1].bytes, w, h) : std::vector<unsigned char>{};
    check("1920x1080 maxSide=1568 retains and encodes 1568x882", cap.event.ok &&
        snapshots.size() == 1 && snapshots[0]->width == 1568 && snapshots[0]->height == 882 &&
        w == 1568 && h == 882 && decoded.size() == 1568 * 882 * 4);
    resized = {}; // Release the large retained snapshot before budget cases below.

    auto rejected = [&](const tc::Pixels& input, int maxSide, const std::string& label) {
        cap.calls = cap.events = 0;
        ImageOptions o; o.maxSide = maxSide;
        auto r = picture(c, input, o);
        c.request(r);
        check(label, local(cap) && detail::VisionAccess::images(r).empty() && charges(r).first == 0);
    };
    rejected(hd, 0, "1920x1080 maxSide=0 fails locally without transport or snapshot");
    rejected(hd, 2000, "maxSide above API maximum does not bypass output limits");
    rejected(hd, -1, "negative maxSide fails locally");
    auto narrow = pixels(4, 1920, 300);
    rejected(narrow, 1568, "downscale producing a side below 280 fails locally");
    auto small = pixels(4, 279, 300);
    rejected(small, 1568, "maxSide never upscales undersized input");
    rejected(hd, 1, "tiny positive maxSide fails locally");

    struct Shape { int width, height, maxSide, outWidth, outHeight; };
    for (const auto shape : {Shape{1001, 777, 701, 701, 544}, Shape{777, 1001, 701, 544, 701},
                             Shape{1000, 561, 500, 500, 281}, Shape{559, 1000, 500, 280, 500},
                             Shape{281, 283, 1568, 281, 283}, Shape{281, 283, 0, 281, 283}}) {
        auto input = pixels(3, shape.width, shape.height);
        ImageOptions o; o.maxSide = shape.maxSide; o.encoding = ImageEncoding::PNG;
        c.request(picture(c, input, o));
        parts = readMultipart(cap.http);
        decoded = parts.size() == 2 ? decode(parts[1].bytes, w, h) : std::vector<unsigned char>{};
        check("odd/portrait/half-up rounding and no upscale " + std::to_string(shape.width) + "x" + std::to_string(shape.height) +
            " maxSide=" + std::to_string(shape.maxSide), cap.event.ok && !decoded.empty() && w == shape.outWidth && h == shape.outHeight);
    }

    options.maxSide = 280;
    for (int channels : {1, 2, 3, 4}) {
        auto flat = pixels(channels, 560, 560);
        for (size_t i = 0; i < flat.getTotalBytes(); i += size_t(channels)) {
            flat.getData()[i] = 37;
            if (channels >= 3) { flat.getData()[i + 1] = 91; flat.getData()[i + 2] = 173; }
            if (channels == 2 || channels == 4) flat.getData()[i + size_t(channels - 1)] = 128;
        }
        c.request(picture(c, flat, options));
        parts = readMultipart(cap.http);
        decoded = parts.size() == 2 ? decode(parts[1].bytes, w, h) : std::vector<unsigned char>{};
        bool staysFlat = cap.event.ok && w == 280 && h == 280 && decoded.size() == rawBytes;
        for (size_t i = 0; i < decoded.size(); i += 4)
            staysFlat &= decoded[i] == 37 && decoded[i + 1] == (channels >= 3 ? 91 : 37) &&
                decoded[i + 2] == (channels >= 3 ? 173 : 37) && decoded[i + 3] == (channels == 2 || channels == 4 ? 128 : 255);
        check("area downscale keeps flat color/alpha channels=" + std::to_string(channels), staysFlat);
    }
    auto checker = pixels(1, 560, 560);
    for (int y = 0; y < 560; ++y) for (int x = 0; x < 560; ++x)
        checker.getData()[y * 560 + x] = (x + y) % 2 ? 255 : 0;
    c.request(picture(c, checker, options));
    parts = readMultipart(cap.http);
    decoded = parts.size() == 2 ? decode(parts[1].bytes, w, h) : std::vector<unsigned char>{};
    bool averaged = cap.event.ok && decoded.size() == rawBytes;
    for (size_t i = 0; i < decoded.size(); i += 4)
        averaged &= decoded[i] == 128 && decoded[i + 1] == 128 && decoded[i + 2] == 128 && decoded[i + 3] == 255;
    check("every 2x2 checker block area-averages to 128", averaged);

    auto odd = pixels(1, 561, 561);
    std::memset(odd.getData(), 0, odd.getTotalBytes());
    for (int y = 0; y < 561; ++y) odd.getData()[y * 561 + 560] = 255;
    c.request(picture(c, odd, options));
    parts = readMultipart(cap.http);
    decoded = parts.size() == 2 ? decode(parts[1].bytes, w, h) : std::vector<unsigned char>{};
    check("odd area coverage includes fractional edge pixels", cap.event.ok && decoded.size() == rawBytes &&
        decoded[0] == 0 && decoded[(280 - 2) * 4] == 0 && decoded[(280 - 1) * 4] == 127);

    for (auto alpha : {AlphaMode::Straight, AlphaMode::Premultiplied}) {
        auto edge = pixels(4, 560, 560);
        for (int y = 0; y < 560; ++y) for (int x = 0; x < 560; ++x) {
            auto* pixel = edge.getData() + (y * 560 + x) * 4;
            const bool opaque = x % 2 == 0;
            pixel[0] = opaque ? 255 : 0; pixel[1] = 0;
            pixel[2] = !opaque && alpha == AlphaMode::Straight ? 255 : 0;
            pixel[3] = opaque ? 255 : 0;
        }
        ImageOptions o = options; o.alpha = alpha;
        c.request(picture(c, edge, o));
        parts = readMultipart(cap.http);
        decoded = parts.size() == 2 ? decode(parts[1].bytes, w, h) : std::vector<unsigned char>{};
        check("area averaging preserves alpha coverage without transparent color halos mode=" + std::to_string(int(alpha)),
            cap.event.ok && decoded.size() == rawBytes && decoded[0] == 255 && decoded[1] == 0 && decoded[2] == 0 && decoded[3] == 128);
    }

    auto rgba = pixels(4, 560, 560);
    ImageOptions automatic; automatic.maxSide = 280;
    for (bool transparent : {false, true}) {
        if (transparent) rgba.getData()[rgba.getTotalBytes() - 1] = 254;
        c.request(picture(c, rgba, automatic));
        parts = readMultipart(cap.http);
        check(transparent ? "resized RGBA with one nonopaque source pixel chooses PNG despite alpha rounding" :
            "resized opaque RGBA chooses JPEG", cap.event.ok && parts.size() == 2 &&
            parts[1].headers.find(transparent ? "image/png" : "image/jpeg") != std::string::npos);
    }

    // Retention and pending admission both use the resized raw-channel payload.
    // The caller's source and temporary encoder buffers are outside these ledgers.
    c.setImageLimits({rawBytes, rawBytes});
    auto budgeted = picture(c, hd, options); // 280x158 is intentionally rejected.
    check("invalid resized shape consumes no budget", detail::VisionAccess::images(budgeted).empty() && charges(budgeted).first == 0);
    budgeted = picture(c, rgba, automatic);
    auto copy = budgeted;
    check("resized snapshot exact-fit budget shared once by Request copies", charges(copy).first == rawBytes);
    check("second resized snapshot exceeds retention budget", !c.imageRequest().addImage(rgba, automatic) && charges(copy).first == rawBytes);
    bool pendingResized = false;
    c.setTransport([&](const TransportRequest&) {
        pendingResized = charges(budgeted) == std::make_pair(rawBytes, rawBytes);
        return success();
    });
    c.request(budgeted);
    check("pending budget charges resized payload during transport and releases on completion", cap.event.ok && pendingResized &&
        charges(budgeted) == std::make_pair(rawBytes, size_t(0)));
    c.setImageLimits({rawBytes, rawBytes - 1});
    const int events = cap.events;
    check("resized pending payload rejects at one byte below exact fit without event", c.request(budgeted) == 0 && cap.events == events && charges(budgeted).second == 0);
    budgeted = {}; copy = {};
    c.setImageLimits({rawBytes - 1, rawBytes});
    auto over = c.imageRequest();
    check("resized snapshot rejects at one byte below exact fit before storage", !over.addImage(rgba, automatic) &&
        detail::VisionAccess::images(over).empty() && charges(over).first == 0);
}
void codecAllocationFailures(Check check) {
    struct Failure { ImageEncoding format; size_t mallocCall, reallocCall; };
    // PNG: filtered pixels, row, hash table, final PNG; first and later growth.
    // JPEG has stack-only compressor scratch, so test its encoded-output growth.
    for (const auto failure : {Failure{ImageEncoding::PNG, 1, 0}, {ImageEncoding::PNG, 2, 0},
                              {ImageEncoding::PNG, 3, 0}, {ImageEncoding::PNG, 4, 0},
                              {ImageEncoding::PNG, 0, 1}, {ImageEncoding::PNG, 0, 2},
                              {ImageEncoding::PNG, 0, 16}, {ImageEncoding::JPEG, 0, 1},
                              {ImageEncoding::JPEG, 0, 2}}) {
        Client c;
        Capture cap; cap.install(c);
        c.setMaxRetries(3);
        ImageOptions options; options.encoding = failure.format;
        auto r = picture(c, pixels(), options);
        detail::setCodecAllocationFailureForTest(failure.mallocCall, failure.reallocCall);
        const auto id = c.request(r);
        const auto label = std::string(failure.format == ImageEncoding::PNG ? "PNG" : "JPEG") +
            " allocator malloc=" + std::to_string(failure.mallocCall) + " realloc=" + std::to_string(failure.reallocCall);
        check(label + " fails once locally without transport/retry", id != 0 &&
            detail::codecAllocationFailureTriggeredForTest() && local(cap) && cap.event.requestId == id);
        check(label + " releases all scratch and pending reservations", detail::codecLiveAllocationsForTest() == 0 &&
            c.getPendingCount() == 0 && charges(r) == std::make_pair(rawBytes, size_t(0)));
        detail::setCodecAllocationFailureForTest(0, 0);
        cap.calls = cap.events = 0;
        c.request(r);
        check(label + " permits a subsequent successful encode", cap.calls == 1 && cap.events == 1 &&
            cap.event.ok && detail::codecLiveAllocationsForTest() == 0 && charges(r).second == 0);
    }
}

void requestAndEncoding(Check check) {
    Client c;
    Capture cap; cap.install(c);
    auto p = pixels();
    for (int channels : {1, 2, 3, 4}) {
        auto input = pixels(channels);
        for (bool transparent : {false, true}) {
            if (transparent && channels != 2 && channels != 4) continue;
            if (transparent) input.getData()[size_t(channels - 1)] = 254; // Exactly one nonopaque pixel.
            auto r = picture(c, input);
            c.request(r);
            auto parts = readMultipart(cap.http);
            bool png = transparent;
            check("Auto channels=" + std::to_string(channels) + (transparent ? " one transparent -> PNG" : " opaque -> JPEG"),
                cap.event.ok && parts.size() == 2 &&
                parts[1].headers.find(png ? "image/png" : "image/jpeg") != std::string::npos &&
                (png ? parts[1].bytes.substr(0, 8) == std::string("\x89PNG\r\n\x1a\n", 8) : parts[1].bytes.substr(0, 2) == std::string("\xff\xd8", 2)));
        }
    }
    auto r = c.imageRequest(); auto a = r.addImage(p);
    r.state(Json{{"nested", Json::array({a, "inline " + a.ref() + a.ref() + "!"})}})
        .model("jev-1.14.0").box("mug", "The mug in `nested`");
    auto j = r.toJson();
    check("Asset ADL and multipart manifest/Box no criteria", j["state"]["nested"][0] == a.ref() &&
        j["assets"][a.ref()] == Json{{"type", "image"}, {"part", "image_0"}} &&
        j["questions"]["mug"] == Json{{"type", "box"}, {"instructions", "The mug in `nested`"}} && j["model"] == "jev-1.14.0");
    c.request(r);
    auto parts = readMultipart(cap.http);
    check("multipart exact headers/manifest and closing CRLF", parts.size() == 2 &&
        parts[0].headers == "Content-Disposition: form-data; name=\"request\"\r\nContent-Type: application/json" &&
        parts[1].headers == "Content-Disposition: form-data; name=\"image_0\"; filename=\"image_0.jpg\"\r\nContent-Type: image/jpeg" &&
        Json::parse(parts[0].bytes) == j && parts[1].bytes.find('\0') != std::string::npos);
    check("danger-zone header only in asset upload", cap.http.headers == std::map<std::string, std::string>{{"TypeSafe-Danger-Zone-Images", "true"}});
    for (const auto& text : {std::string("tcxjev-asset-malformed"), a.ref(), a.ref() + "!"}) {
        Request plain; plain.state(text).noul("q", "tcxjev-asset-foreign");
        c.request(plain);
        const auto expected = std::string("{\"model\":\"jev-latest\",\"questions\":{\"q\":{\"instructions\":\"tcxjev-asset-foreign\",\"type\":\"noul\"}},\"state\":\"") + text + "\"}";
        check("text request pre-change bytes and no header", cap.http.body == expected && cap.http.headers.empty() && cap.http.contentType == "application/json");
        c.request(text, Json{{"q", {{"type", "noul"}, {"instructions", "tcxjev-asset-foreign"}}}});
        check("raw text entry preserves reserved-prefix bytes", cap.http.body == expected && cap.http.headers.empty());
    }
    auto copy = r;
    c.request(copy);
    check("Request copies share image identities", cap.event.ok && charges(r).first == rawBytes);
    auto before = readMultipart(cap.http)[1].bytes;
    std::memset(p.getData(), 0, p.getTotalBytes());
    c.request(copy);
    check("source mutation cannot alter retained snapshot", readMultipart(cap.http)[1].bytes == before);

    for (const auto& field : {"state", "instructions", "criteria"}) {
        auto q = c.imageRequest(); auto image = q.addImage(p);
        Json nested{{"values", Json::array({"prefix " + image.ref() + " suffix", image})}};
        q.state(field == std::string("state") ? nested : Json("state"));
        q.question("q", {{"type", "noul"}, {"instructions", field == std::string("instructions") ? nested : Json("q")},
            {"criteria", field == std::string("criteria") ? nested : Json::object()}});
        c.request(q);
        check(std::string("reference traversal in ") + field, cap.event.ok);
    }
    // Exercise collision regeneration and NUL preservation through the internal packer.
    std::vector<detail::EncodedImage> fake{{std::string("collision\0tail", 14), false}};
    auto one = picture(c, p);
    TransportRequest packed;
    int boundaries = 0;
    detail::packVision(one, one.toJson(), fake, packed, [&] { return ++boundaries == 1 ? "collision" : "safe-boundary"; });
    auto fakeParts = readMultipart(packed);
    check("boundary collision regenerates and binary NUL survives", boundaries == 2 && fakeParts.size() == 2 && fakeParts[1].bytes == fake[0].bytes);

    // Explicit PNG, premultiplied linear RGB and a linear-light JPEG matte.
    auto color = pixels();
    for (size_t i = 0; i < color.getTotalBytes(); i += 4) {
        color.getData()[i] = 64; color.getData()[i + 1] = 0; color.getData()[i + 2] = 0; color.getData()[i + 3] = 128;
    }
    ImageOptions options; options.encoding = ImageEncoding::PNG; options.alpha = AlphaMode::Premultiplied; options.colorSpace = ImageColorSpace::Linear;
    c.request(picture(c, color, options));
    int w, h;
    auto d = decode(readMultipart(cap.http)[1].bytes, w, h);
    check("unpremultiply before linear-to-sRGB and preserve alpha", d.size() == rawBytes && std::abs(int(d[0]) - 188) <= 1 && d[1] == 0 && d[3] == 128);
    options.encoding = ImageEncoding::JPEG; options.colorSpace = ImageColorSpace::SRGB; options.alpha = AlphaMode::Straight;
    options.jpegBackground = tc::Color(1, 1, 1, 1);
    std::memset(color.getData(), 0, color.getTotalBytes());
    for (size_t i = 3; i < color.getTotalBytes(); i += 4) color.getData()[i] = 128;
    c.request(picture(c, color, options));
    d = decode(readMultipart(cap.http)[1].bytes, w, h);
    check("explicit JPEG composites alpha over matte in linear light", d.size() == rawBytes && std::abs(int(d[0]) - 187) < 5 && d[3] == 255);
}
void orientation(Check check) {
    setCoreImageFlip(1);
    auto p = pixels();
    for (int y = 0; y < 280; ++y) for (int x = 0; x < 280; ++x) {
        auto* color = p.getData() + (size_t(y) * 280 + size_t(x)) * 4;
        color[0] = y < 140 ? 255 : 0; color[1] = 0; color[2] = y < 140 ? 0 : 255;
    }
    for (auto encoding : {ImageEncoding::PNG, ImageEncoding::JPEG}) {
        for (auto origin : {ImageOrigin::TopLeft, ImageOrigin::BottomLeft}) {
            Client c; Capture cap; cap.install(c);
            ImageOptions opts; opts.encoding = encoding; opts.origin = origin;
            c.request(picture(c, p, opts));
            int w, h; auto d = decode(readMultipart(cap.http)[1].bytes, w, h);
            size_t top = (size_t(10) * 280 + 10) * 4, bottom = (size_t(270) * 280 + 10) * 4;
            bool upright = d.size() == rawBytes && w == 280 && h == 280;
            if (upright && origin == ImageOrigin::TopLeft) upright = d[top] > 245 && d[top + 2] < 10 && d[bottom + 2] > 245;
            else if (upright) upright = d[top + 2] > 245 && d[bottom] > 245;
            check(std::string("core flip=1 isolated: ") + (encoding == ImageEncoding::PNG ? "PNG" : "JPEG") + (origin == ImageOrigin::TopLeft ? " top-left" : " bottom-left"), upright);
        }
    }
    // Concurrent preparation under the same stable core flag.
    std::vector<std::future<bool>> work;
    for (int i = 0; i < 4; ++i) work.push_back(std::async(std::launch::async, [&] {
        Client c; Capture cap; cap.install(c);
        c.request(picture(c, p));
        int w, h; auto d = decode(readMultipart(cap.http)[1].bytes, w, h);
        return d.size() == rawBytes && d[0] > 245 && d[2] < 10;
    }));
    bool good = true; for (auto& task : work) good &= task.get();
    check("concurrent addon JPEG encodes ignore stable core flip", good);
    int w, h; auto d = decode(corePng(p), w, h);
    check("addon leaves core's own flipped output intact", d.size() == rawBytes && d[0] == 0 && d[2] == 255);
    setCoreImageFlip(0);
}
void validation(Check check) {
    auto p = pixels();
    auto run = [&](const std::string& name, const std::function<Request(Client&)>& make) {
        Client c; Capture cap; cap.install(c);
        auto r = make(c);
        auto id = c.request(r);
        check(name, id && local(cap) && c.getPendingCount() == 0 && charges(c.imageRequest()).second == 0);
    };
    run("unattached Asset handle rejected locally without retries", [&](Client& c) {
        auto first = c.imageRequest(); auto a = first.addImage(p);
        auto second = c.imageRequest(); second.state(Json{{"foreign", a}}).noul("q", "q"); return second;
    });
    run("unused Asset (object keys don't count) rejected", [&](Client& c) {
        auto r = c.imageRequest(); auto a = r.addImage(p);
        r.state(Json{{a.ref(), "not a use"}}).noul("q", "q"); return r;
    });
    for (auto token : {"tcxjev-asset-malformed", "tcxjev-asset-00000000000000000000000000000000"})
        run("unknown or malformed reserved reference rejected", [&](Client& c) { auto r = c.imageRequest(); r.state(token).noul("q", "q"); return r; });
    run("default Request cannot register images", [&](Client&) {
        Request r; auto a = r.addImage(p); check("failed Asset is false and empty", !a && a.ref().empty()); r.state("x").noul("q", "q"); return r;
    });
    run("assetless raw Box rejected", [&](Client&) { Request r; r.state("x").question("b", {{"type", "box"}, {"instructions", "q"}}); return r; });
    run("known 1.13 Box model rejected", [&](Client& c) { auto r = picture(c, p); r.model("jev-1.13.0").box("b", "q"); return r; });
    run("Box criteria rejected", [&](Client& c) { auto r = picture(c, p); r.question("b", {{"type", "box"}, {"instructions", "q"}, {"criteria", nullptr}}); return r; });
    for (auto size : {279, 1569}) run("out-of-range dimensions fail before snapshot", [&](Client& c) {
        auto input = pixels(4, size, 280); auto r = picture(c, input);
        check("invalid dimensions retained no snapshot", charges(r).first == 0); return r;
    });
    run("F32 rejected before snapshot", [&](Client& c) { tc::Pixels f; f.allocate(280, 280, 4, tc::PixelFormat::F32); return picture(c, f); });
    run("empty pixels rejected", [&](Client& c) { tc::Pixels empty; return picture(c, empty); });
    for (int quality : {0, 101}) run("invalid JPEG quality rejected", [&](Client& c) { ImageOptions o; o.jpegQuality = quality; return picture(c, p, o); });
    run("nonfinite JSON rejected without content in error", [&](Client& c) { auto r = picture(c, p); r.question("bad", {{"type", "noul"}, {"instructions", std::nan("")}}); return r; });
    run("invalid UTF-8 rejected before transport", [&](Client& c) { auto r = picture(c, p); r.noul("bad", std::string("private-content\xff", 16)); return r; });
    for (int levels : {64, 65}) {
        Client c; Capture cap; cap.install(c);
        auto r = picture(c, p);
        Json j = "leaf"; for (int i = 0; i < levels; ++i) j = Json::array({j});
        r.noul("deep", j); c.request(r);
        check("JSON nesting level " + std::to_string(levels), levels == 64 ? cap.event.ok : local(cap));
    }
    Client c; Capture cap; cap.install(c);
    auto r = picture(c, p); auto id = detail::VisionAccess::images(r)[0]->reference;
    r.state(std::string(108, 'x'));
    std::string repeated; for (int i = 0; i < 108; ++i) repeated += id;
    r.state(repeated); c.request(r);
    check("each repeated inline reference counts toward decoded expanded guard", local(cap) && cap.event.error.find("decoded expanded") != std::string::npos);
    size_t out = 0;
    check("checked product rejects size_t overflow", !detail::checkedMultiply((std::numeric_limits<size_t>::max)(), 2, out));
    out = (std::numeric_limits<size_t>::max)();
    check("checked sum rejects overflow and keeps old total", !detail::checkedAdd(out, 1, out) && out == (std::numeric_limits<size_t>::max)());

    // Large limits use synthetic encoded bytes; all production guards remain active.
    auto single = picture(c, p);
    for (size_t size : {16 * MiB, 16 * MiB + 1}) {
        bool accepted = true;
        try { TransportRequest h; detail::packVision(single, single.toJson(), {{std::string(size, 'x'), false}}, h); }
        catch (...) { accepted = false; }
        check("encoded file limit inclusive " + std::to_string(size), accepted == (size == 16 * MiB));
    }
    auto two = c.imageRequest(); auto a = two.addImage(p); auto b = two.addImage(p);
    two.state(Json::array({a, b})).noul("q", "q");
    auto json = two.toJson(); json.erase("assets");
    auto text = json.dump().size() - a.ref().size() - b.ref().size();
    bool rejected = false;
    try {
        TransportRequest h;
        detail::packVision(two, two.toJson(), {{std::string(16 * MiB, 'x'), false}, {std::string(16 * MiB - text, 'y'), true}}, h);
    } catch (const std::exception& e) { rejected = std::string(e.what()).find("multipart HTTP body") != std::string::npos; }
    check("final multipart framing makes exact-expanded-limit content too large", rejected);
    auto many = c.imageRequest(); Json refs = Json::array();
    auto big = pixels(1, 1568, 1568);
    for (int i = 0; i < 4; ++i) refs.push_back(many.addImage(big));
    many.state(refs).noul("q", "q"); c.request(many);
    check("distinct decoded addon guard rejects locally", cap.event.error.find("decoded distinct") != std::string::npos && cap.event.statusCode == 0);
}
void budgets(Check check) {
    auto p = pixels();
    Client c; Capture cap; cap.install(c);
    c.setImageLimits({rawBytes, rawBytes});
    auto r = picture(c, p);
    auto copy = r;
    check("snapshot exact fit and Request copy charged once", charges(r).first == rawBytes);
    auto blocked = c.imageRequest(); auto invalid = blocked.addImage(p);
    check("cumulative never-submitted builders exhaust budget before copy", !invalid && charges(r).first == rawBytes && detail::VisionAccess::images(blocked).empty());
    c.request(blocked.state("x").noul("q", "q"));
    check("snapshot rejection becomes exactly one sticky local event", local(cap));
    c.request(r);
    check("completion releases pending while surviving builders retain snapshot", charges(r) == std::make_pair(rawBytes, size_t(0)));
    r = Request();
    check("last builder copy still retains snapshot", charges(copy).first == rawBytes);
    copy = Request();
    auto probe = c.imageRequest();
    check("final snapshot owner releases ledger", charges(probe).first == 0);
    c.setImageLimits({rawBytes - 1, rawBytes});
    check("one-byte snapshot budget excess fails", !probe.addImage(p));
    c.setImageLimits({rawBytes, rawBytes - 1});
    auto pending = picture(c, p);
    int events = cap.events, calls = cap.calls;
    check("pending budget rejects before acceptance and encoding", c.request(pending) == 0 && cap.events == events && cap.calls == calls && charges(pending).second == 0);
    c.setImageLimits({0, 0});
    check("lowering budgets preserves live snapshot and blocks new work", charges(pending).first == rawBytes && c.request(pending) == 0 && !c.imageRequest().addImage(p));
    c.setImageLimits({rawBytes, rawBytes});
    c.request(pending); c.request(pending);
    check("sync repeated submission returns pending charge each time", charges(pending).second == 0 && cap.events == events + 2);
    // Transport failure is terminal ownership release too, independent of builder lifetime.
    pending = Request();
    c.setTransport([](const TransportRequest&) { return TransportResponse{413, "{}", {}}; });
    c.request(picture(c, p));
    check("HTTP failure releases job-only snapshot and pending reservation", charges(probe) == std::make_pair(size_t(0), size_t(0)));
    c.setTransport([&](const TransportRequest&) { return success(); });
    pending = picture(c, p);
    Client other; Capture otherCap; otherCap.install(other);
    check("cross-Client builder rejected before retention or transport", other.request(pending) == 0 && otherCap.calls == 0 && otherCap.events == 0);
    pending = Request();

    // A blocked transport holds one job, one queued job uses the remaining budget.
    Client async;
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    std::atomic<int> reached{0}, fired{0};
    auto caller = std::this_thread::get_id();
    std::atomic<bool> workerThread{false};
    async.setTransport([&](const TransportRequest&) {
        workerThread = std::this_thread::get_id() != caller;
        reached.fetch_add(1);
        cv.notify_all();
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return open; });
        return success();
    }).setMaxConcurrent(1).setImageLimits({3 * rawBytes, 2 * rawBytes});
    auto listener = async.responseEvent.listen([&](ResponseEventArgs&) { ++fired; });
    auto one = picture(async, p);
    auto first = async.request(one);
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return reached.load() == 1; });
    }
    auto second = async.request(one);
    check("pending counts raw bytes per repeated submission, even shared snapshots", first && second && charges(one).second == 2 * rawBytes && async.request(one) == 0);
    check("queued cancellation returns its charge", async.cancel(second) && charges(one).second == rawBytes && fired == 1);
    auto third = async.request(one);
    check("cancelAll releases queue charges before events", third && async.cancelAll() == 1 && charges(one).second == rawBytes);
    one = Request();
    auto watch = async.imageRequest();
    check("active job retains snapshot after builder destruction", charges(watch).first == rawBytes);
    {
        std::lock_guard<std::mutex> lock(m); open = true;
    }
    cv.notify_all();
    for (int i = 0; i < 5000 && async.getPendingCount(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check("async completion releases final snapshot and pending charges", charges(watch) == std::make_pair(size_t(0), size_t(0)) && async.getPendingCount() == 0 && workerThread);

    auto owner = std::make_unique<Client>();
    owner->setTransport([](const TransportRequest&) { return success(); }).setAsync(false);
    auto survivor = picture(*owner, p);
    owner.reset();
    check("destroyed Client closes surviving builder admission", !survivor.addImage(p) && charges(survivor).first == rawBytes);
    auto ledger = detail::VisionAccess::budget(survivor);
    survivor = Request();
    check("snapshot ledger safely outlives Client and returns charge", ledger->snapshots == 0 && ledger->pending == 0 && !ledger->open);

    // Destroy while one image job is active and another is queued.
    std::mutex destroyMutex;
    std::condition_variable destroyCv;
    bool unblock = false;
    std::atomic<bool> active{false};
    auto doomed = std::make_unique<Client>();
    doomed->setMaxConcurrent(1).setTransport([&](const TransportRequest&) {
        active = true; destroyCv.notify_all();
        std::unique_lock<std::mutex> lock(destroyMutex);
        destroyCv.wait_for(lock, std::chrono::seconds(10), [&] { return unblock; });
        return success();
    });
    auto retained = picture(*doomed, p);
    auto closedLedger = detail::VisionAccess::budget(retained);
    doomed->request(retained);
    {
        std::unique_lock<std::mutex> lock(destroyMutex);
        destroyCv.wait_for(lock, std::chrono::seconds(5), [&] { return active.load(); });
    }
    doomed->request(retained);
    retained = Request();
    auto destroy = std::async(std::launch::async, [&] { doomed.reset(); });
    bool sawClosed = false;
    for (int i = 0; i < 5000 && !sawClosed; ++i) {
        {
            std::lock_guard<std::mutex> lock(closedLedger->mutex);
            sawClosed = !closedLedger->open;
        }
        if (!sawClosed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard<std::mutex> lock(destroyMutex); unblock = true;
    }
    destroyCv.notify_all(); destroy.get();
    check("Client destruction releases queued and active image ownership", sawClosed && closedLedger->pending == 0 && closedLedger->snapshots == 0);

    // Concurrent registrations cannot overbook a one-image budget.
    Client concurrent; concurrent.setTransport([](const TransportRequest&) { return success(); });
    concurrent.setImageLimits({rawBytes, rawBytes});
    auto a = concurrent.imageRequest(), b = concurrent.imageRequest();
    auto fa = std::async(std::launch::async, [&] { return bool(a.addImage(p)); });
    auto fb = std::async(std::launch::async, [&] { return bool(b.addImage(p)); });
    bool aa = fa.get(), bb = fb.get();
    check("snapshot reservations are atomic across concurrent builders", aa != bb && charges(a).first == rawBytes);
}
void boxesAndErrors(Check check) {
    // Published Box guide fixture, retrieved 2026-10-21. NOT a live capture.
    Json fixture = Json::parse(R"({"model":"jev-1.14.0","answers":{"mug":{"type":"box","box":{"top":0.25,"bottom":0.82,"left":0.44,"right":0.84},"confidence":0.92}},"usage":{"input_tokens":290,"output_tokens":21}})");
    Client c; c.setAsync(false);
    ResponseEventArgs event;
    auto listener = c.responseEvent.listen([&](ResponseEventArgs& e) { event = e; });
    auto submit = [&](const Json& j) {
        c.setTransport([j](const TransportRequest&) { return TransportResponse{200, j.dump(), {}}; });
        c.request(Request().state("x").noul("q", "q"));
    };
    submit(fixture);
    auto box = event.answers["mug"].box;
    check("published 1.14 Box fixture parses normalized Rect", event.ok && box && event.answers["mug"].type == QuestionType::Box &&
        std::abs(box->rect.x - .44) < 1e-6 && std::abs(box->rect.y - .25) < 1e-6 &&
        std::abs(box->rect.width - .4) < 1e-6 && std::abs(box->rect.height - .57) < 1e-6 && box->confidence == .92);
    if (box) {
        auto rect = box->toPixels(1000, 500);
        check("Box toPixels preserves fractional pixels/top-left origin", std::abs(rect.x - 440) < .001 && std::abs(rect.y - 125) < .001 &&
            std::abs(rect.width - 400) < .001 && std::abs(rect.height - 285) < .001);
    }
    for (const Json& confidence : {Json(nullptr), Json(0.0), Json(.92)}) {
        fixture["answers"]["mug"]["confidence"] = confidence;
        submit(fixture);
        box = event.answers["mug"].box;
        check("constructed nullable/zero/numeric Box confidence", event.ok && box &&
            (confidence.is_null() ? !box->confidence : box->confidence && *box->confidence == confidence.get<double>()));
    }
    fixture["answers"]["mug"].erase("confidence"); submit(fixture);
    check("missing Box confidence stays absent", event.ok && event.answers["mug"].box && !event.answers["mug"].box->confidence);
    auto valid = fixture;
    for (const auto& bad : {Json(-.1), Json(1.1), Json("bad"), Json(nullptr)}) {
        auto malformed = valid; malformed["answers"]["mug"]["box"]["left"] = bad;
        submit(malformed);
        check("invalid Box edge fails response and retains raw", !event.ok && event.answers.empty() && event.raw == malformed && event.error.find("invalid Box") != std::string::npos);
    }
    auto reversed = valid; reversed["answers"]["mug"]["box"]["left"] = .9; submit(reversed);
    check("reversed Box edges rejected", !event.ok);
    auto equal = valid; equal["answers"]["mug"]["box"]["left"] = .84; submit(equal);
    check("zero-width Box rejected", !event.ok);
    for (const auto& bad : {Json(-.1), Json(1.1), Json("bad")}) {
        auto malformed = valid; malformed["answers"]["mug"]["confidence"] = bad; submit(malformed);
        check("invalid Box confidence rejected", !event.ok);
    }
    auto mixed = valid;
    mixed["answers"]["yes"] = {{"type", "noul"}, {"noul", .75}};
    submit(mixed);
    check("mixed old and Box answers parse together", event.ok && event.answers["yes"].noul == .75 && event.answers["mug"].box);
    for (int status : {400, 413, 415, 422}) {
        // Constructed error fixtures. Auditor captures remain required for compatibility.
        int attempts = 0;
        const Json error{{"detail", {{"error_type", status == 400 ? "max_tokens_exceeded" : "image_error"}, {"message", "rejected"}}}};
        c.setTransport([&](const TransportRequest&) { ++attempts; return TransportResponse{status, error.dump(), {}}; });
        c.request(Request().state("x").noul("q", "q"));
        check("image/API error " + std::to_string(status) + " preserves raw/type without retry", !event.ok && event.raw == error && event.statusCode == status && attempts == 1 &&
            event.error.find(status == 400 ? "max_tokens_exceeded" : "image_error") != std::string::npos);
    }
    auto p = pixels();
    TransportRequest original;
    int attempts = 0;
    bool same = true;
    c.setRetryDelay(0).setTransport([&](const TransportRequest& h) {
        if (!attempts++) { original = h; return TransportResponse{503, "{}", {}}; }
        same = original.body == h.body && original.headers == h.headers && original.contentType == h.contentType;
        return success();
    });
    c.request(picture(c, p));
    check("image retries reuse identical encoded bytes/boundary/headers", attempts == 2 && same && event.ok);
}
} // namespace

// Keep a deadline even when CI runs the binary directly instead of using CTest.
void testAdmissionReentrancy(Check check) {
    struct Deadline {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
        std::thread watchdog{[this] {
            std::unique_lock<std::mutex> lock(mutex);
            if (cv.wait_for(lock, std::chrono::seconds(5), [this] { return finished; })) return;
            std::fputs("FAIL: admission listener reentrancy timed out\n", stderr);
            std::fflush(stderr);
            std::_Exit(1); // Do not unwind or join a deadlocked Client.
        }};
        ~Deadline() {
            { std::lock_guard<std::mutex> lock(mutex); finished = true; }
            cv.notify_one();
            watchdog.join();
        }
    } deadline;
    for (bool async : {false, true}) {
        Client c;
        Capture cap; cap.install(c);
        c.setAsync(async).setImageLimits({rawBytes, 1});
        auto r = picture(c, pixels());
        bool queried = false, changed = false;
        auto listener = tc::getLogger().onLog.listen([&](tc::LogEventArgs& e) {
            if (e.message.find("image admission: pending image byte budget exhausted") == std::string::npos) return;
            queried = c.getPendingCount() == 0;
            c.setImageLimits({rawBytes, 1});
            changed = true;
        });
        auto id = c.request(r);
        const auto mode = async ? "async" : "sync";
        check(std::string(mode) + " rejection logger can query Client and change image limits", queried && changed);
        check(std::string(mode) + " reentrant rejection returns zero without transport/event/charge",
            id == 0 && cap.calls == 0 && cap.events == 0 && c.getPendingCount() == 0 && charges(r).second == 0);
    }
}

void testVision(Check check) {
    groupedLocale(check);
    codecAllocationFailures(check);
    downscaling(check);
    requestAndEncoding(check);
    orientation(check);
    validation(check);
    budgets(check);
    boxesAndErrors(check);
}
