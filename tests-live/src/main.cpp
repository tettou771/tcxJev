#include <tcxJev.h>
#include <tcxCurl.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <locale>
#include <stdexcept>

using namespace tcx::jev;
namespace {
bool selfTest = false;
bool selfTestErrors = false;
std::string imagePath;
int exitCode = 0;
struct Upload {
    bool invoked = false;
    size_t bytes = 0;
    int width = 0, height = 0;
    std::string format = "-";
};

// Inspect only the image part, never authorization or other request headers.
// Bytes counts the encoded image file, excluding multipart framing and JSON.
void measure(const TransportRequest& h, Upload& u) {
    u.invoked = true;
    const std::string prefix = "multipart/form-data; boundary=";
    if (h.contentType.compare(0, prefix.size(), prefix)) throw std::runtime_error("multipart");
    const auto boundary = h.contentType.substr(prefix.size());
    const auto image = h.body.find("Content-Disposition: form-data; name=\"image_0\"");
    const auto split = h.body.find("\r\n\r\n", image);
    const auto end = h.body.find("\r\n--" + boundary, split == std::string::npos ? split : split + 4);
    if (image == std::string::npos || split == std::string::npos || end == std::string::npos)
        throw std::runtime_error("multipart");
    const auto mime = h.body.substr(image, split - image);
    u.format = mime.find("image/png") != std::string::npos ? "png" :
               mime.find("image/jpeg") != std::string::npos ? "jpg" : "-";
    u.bytes = end - split - 4;
    // Decode independently in self-test to validate byte boundaries and format.
    if (selfTest) {
        int w = 0, height = 0, channels = 0;
        const auto* data = reinterpret_cast<const unsigned char*>(h.body.data() + split + 4);
        auto* decoded = stbi_load_from_memory(data, static_cast<int>(u.bytes), &w, &height, &channels, 0);
        const bool png = u.bytes >= 8 && data[0] == 137 && data[1] == 'P';
        const bool jpg = u.bytes >= 3 && data[0] == 255 && data[1] == 216;
        const bool valid = decoded && w > 0 && height > 0 &&
            (u.format == "png" ? png : u.format == "jpg" && jpg);
        u.width = w;
        u.height = height;
        stbi_image_free(decoded);
        if (!valid) throw std::runtime_error("encoding");
    }
}

// No public resize helper: area-weighted CPU averaging, keeping channel count.
// Scale the long side to <=200, so the short side is always below 280.
tc::Pixels tinyCopy(const tc::Pixels& source) {
    const int sw = source.getWidth(), sh = source.getHeight(), channels = source.getChannels();
    const double scale = std::min(1.0, 200.0 / std::max(sw, sh));
    const int dw = std::max(1, int(std::lround(sw * scale)));
    const int dh = std::max(1, int(std::lround(sh * scale)));
    tc::Pixels out;
    out.allocate(dw, dh, channels);
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
        const double x0 = double(x) * sw / dw, x1 = double(x + 1) * sw / dw;
        const double y0 = double(y) * sh / dh, y1 = double(y + 1) * sh / dh;
        for (int c = 0; c < channels; ++c) {
            double sum = 0;
            for (int sy = int(y0); sy < std::min(sh, int(std::ceil(y1))); ++sy)
                for (int sx = int(x0); sx < std::min(sw, int(std::ceil(x1))); ++sx)
                    sum += source.getData()[(size_t(sy) * sw + sx) * channels + c] *
                        (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) *
                        (std::min(y1, double(sy + 1)) - std::max(y0, double(sy)));
            out.getData()[(size_t(y) * dw + x) * channels + c] =
                static_cast<unsigned char>(std::lround(sum / ((x1 - x0) * (y1 - y0))));
        }
    }
    return out;
}

// Fixed labels only: never print server-controlled strings, error text or raw JSON.
std::string errorKind(const ResponseEventArgs& e, const Upload& u) {
    if (e.ok) return "-";
    if (!u.invoked && e.error.starts_with("local validation:")) return "local_validation";
    if (!u.invoked) return "admission";
    if (!e.statusCode) return "network";
    if (e.statusCode >= 200 && e.statusCode < 300) return "response_parse";
    if (e.raw.is_object() && e.raw.contains("detail") && e.raw["detail"].is_object()) {
        const auto& detail = e.raw["detail"];
        if (detail.contains("error_type") && detail["error_type"].is_string()) {
            const auto type = detail["error_type"].get<std::string>();
            for (const char* allowed : {"authentication_error", "image_error", "max_tokens_exceeded",
                     "validation_error", "rate_limit_error", "overloaded_error"})
                if (type == allowed) return allowed;
        }
    }
    return "http_" + std::to_string(e.statusCode);
}

struct LiveApp : tc::App {
    // Explicit empty key in self-test: do not even inspect the environment there.
    Client client{selfTest ? std::string{} : std::string(std::getenv("TYPESAFE_API_KEY"))};
    void setup() override {
        tc::Pixels pixels;
        if (selfTest) {
            pixels.allocate(1920, 1080, 3);
            for (size_t i = 0; i < pixels.getTotalBytes(); ++i)
                pixels.getData()[i] = static_cast<unsigned char>((i * 31 + i / 97) % 256);
        } else if (!pixels.load(std::filesystem::absolute(imagePath)) || !pixels.isAllocated()) {
            std::cerr << "ERROR: image is unreadable or unsupported\n";
            exitCode = 2;
            requestExit();
            return;
        }
        client.setAsync(false).setMaxRetries(0).setTimeout(30);
        auto tiny = tinyCopy(pixels);
        size_t firstBytes = 0;
        for (int n = 1; n <= 4; ++n) {
            Upload upload;
            bool transportValid = true, settled = false;
            ResponseEventArgs event;
            client.setTransport([&](const TransportRequest& h) {
                try { measure(h, upload); }
                catch (...) { transportValid = false; return TransportResponse{0, {}, "measurement failed"}; }
                if (selfTest) {
                    if (selfTestErrors) return TransportResponse{401,
                        R"({"detail":{"error_type":"authentication_error","message":"Authorization: Bearer OFFLINE_HEADER_ECHO_SENTINEL"}})", {}};
                    if (n == 3) return TransportResponse{200,
                        R"({"answers":{"tower":{"type":"box","box":{"left":0.1,"top":0.2,"right":0.6,"bottom":0.9},"confidence":null}},"usage":{"input_tokens":123}})", {}};
                    return TransportResponse{200,
                        R"({"answers":{"tower":{"type":"noul","noul":0.75}},"usage":{"input_tokens":123}})", {}};
                }
                // Same HTTPS implementation as the addon's default transport.
                tcx::curl::HttpClient http;
                http.setVerbose(false);
                http.setTimeout(std::max(1L, static_cast<long>(std::ceil(h.timeoutSeconds))));
                http.setBearerToken(h.apiKey);
                for (const auto& [name, value] : h.headers) http.addHeader(name, value);
                auto r = http.postRaw(h.url, h.body, h.contentType);
                // Error text is intentionally discarded; status/body suffice for parsing.
                return TransportResponse{r.statusCode, std::move(r.body), {}};
            });
            auto listener = client.responseEvent.listen([&](ResponseEventArgs& e) { event = e; settled = true; });
            ImageOptions options;
            if (n != 4) options.maxSide = n == 2 ? 640 : 1568;
            if (n == 2) options.jpegQuality = 50;
            auto r = client.imageRequest();
            auto photo = r.addImage(n == 4 ? tiny : pixels, options);
            r.model("jev-1.14.0").state(tc::Json{{"photo", photo}});
            if (n == 3) r.box("tower", "the tower");
            else r.noul("tower", "Does `photo` show a transmission tower?");
            client.request(r);
            const auto kind = errorKind(event, upload);
            if (n == 4 && (upload.invoked || !settled || kind != "local_validation")) exitCode = 1;
            if (selfTest && (!transportValid || !settled || (n < 4 && ((selfTestErrors ? event.statusCode != 401 : !event.ok) || !upload.bytes || upload.format != "jpg"))))
                exitCode = 1;
            if (selfTest && n < 4 && (upload.width != (n == 2 ? 640 : 1568) ||
                upload.height != (n == 2 ? 360 : 882))) exitCode = 1;
            if (n == 1) firstBytes = upload.bytes;
            if (selfTest && n == 2 && upload.bytes >= firstBytes) exitCode = 1;
            std::cout << '#' << n << (event.ok ? " ok" : " err") << " kind=" << kind << " tokens=";
            if (event.raw.is_object() && event.raw.contains("usage") && event.raw["usage"].is_object() &&
                event.raw["usage"].contains("input_tokens") && event.raw["usage"]["input_tokens"].is_number())
                std::cout << event.inputTokens;
            else std::cout << '-';
            std::cout << " answer=";
            const auto answer = event.answers.find("tower");
            if (!event.ok || answer == event.answers.end()) std::cout << '-';
            else if (n != 3) std::cout << answer->second.noul;
            else if (answer->second.box) {
                const auto& b = *answer->second.box;
                std::cout << b.rect.x << ',' << b.rect.y << ',' << b.rect.width << ',' << b.rect.height << " conf=";
                if (b.confidence) std::cout << *b.confidence;
                else std::cout << "null";
            } else std::cout << "null conf=null";
            std::cout << " bytes=" << upload.bytes << " format=" << upload.format;
            // The fourth line also reports the public transport hook observation.
            if (n == 4) std::cout << " transport=" << (upload.invoked ? "invoked" : "not_invoked");
            std::cout << '\n';
        }
        requestExit();
    }
};
} // namespace

int main(int argc, char** argv) {
    tc::setLogLevel(tc::LogLevel::Silent);
    std::cout.imbue(std::locale::classic());
    selfTestErrors = argc == 2 && std::string(argv[1]) == "--self-test-errors";
    selfTest = selfTestErrors || (argc == 2 && std::string(argv[1]) == "--self-test");
    if (!selfTest) {
        const char* key = std::getenv("TYPESAFE_API_KEY");
        if (!key || !*key) {
            std::cout << "SKIP: TYPESAFE_API_KEY not set\n";
            return 0;
        }
        if (argc < 2) {
            std::cerr << "ERROR: supply an image path as argv[1]\n";
            return 2;
        }
        imagePath = argv[1];
    }
    try {
        tc::HeadlessSettings settings;
        tc::runHeadlessApp<LiveApp>(settings);
    } catch (...) {
        // Never echo exception messages: a networking exception may contain headers.
        std::cerr << "ERROR: runner failed\n";
        return 2;
    }
    return exitCode;
}
