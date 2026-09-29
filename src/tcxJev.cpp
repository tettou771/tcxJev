#include "tcxJev.h"

#include <tcxCurl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <random>

#include <thread>

#if !defined(__EMSCRIPTEN__)
#include <condition_variable>
#include <mutex>
#endif

using namespace std;
using namespace tc;

namespace tcx::jev {

namespace {

constexpr const char* kLog = "tcxJev";
constexpr const char* kEndpoint = "https://api.typesafe.ai/v1/systemone";
constexpr const char* kDefaultModel = "jev-latest";
constexpr size_t kMaxChoiceOptions = 255;
constexpr size_t kMinScoreLevels = 2;
constexpr size_t kMaxScoreLevels = 10;

#if defined(__EMSCRIPTEN__)
// Web: no worker threads (and tcxCurl has no web backend), so request() is
// refused there and nothing below ever runs concurrently.
constexpr bool kThreadsAvailable = false;
struct NoMutex {
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }
};
using Mutex = NoMutex;
#else
constexpr bool kThreadsAvailable = true;
using Mutex = std::mutex;
#endif

string trimmed(const string& s) {
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Cut at `n` bytes without splitting a UTF-8 sequence.
string clipped(const string& s, size_t n = 300) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "...";
}

// Replaces every occurrence of the API key, so an error body that echoes it
// can't leak it into a log or an event.
string redactKey(string text, const string& key) {
    if (key.size() < 8) return text;   // too short to be a real key; avoid mangling text
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != string::npos) {
        text.replace(pos, key.size(), "<redacted>");
        pos += 10;
    }
    return text;
}

const char* statusText(int code) {
    switch (code) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized: check the API key";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity: the request failed validation";
        case 429: return "Too Many Requests: rate limited";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 529: return "Overloaded";
        default:  return "";
    }
}

// 408, 429 and 5xx (529 Overloaded included) are worth another try, and so is
// no response at all (connection failure, timeout). Other 4xx are final.
bool isRetryable(int status) {
    return status == 0 || status == 408 || status == 429 || (status >= 500 && status <= 599);
}

float backoffSeconds(int attempt, float initial, float maxDelay) {
    double d = initial * std::pow(2.0, attempt);
    d = (std::min)(d, static_cast<double>(maxDelay));
    thread_local std::minstd_rand rng(static_cast<unsigned>(
        chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<double> jitter(0.0, 0.25);
    return static_cast<float>(d * (1.0 - jitter(rng)));
}

string describeStatus(const TransportResponse& res) {
    if (res.statusCode == 0) {
        return "network error: " + (res.error.empty() ? string("no response") : res.error);
    }
    string s = "HTTP " + to_string(res.statusCode);
    const char* text = statusText(res.statusCode);
    if (text[0]) s += string(" (") + text + ")";
    return s;
}

// --- JSON field access that never throws ------------------------------------

double numberOr(const Json& obj, const char* key, double def = 0) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_number()) ? it->get<double>() : def;
}

string stringOr(const Json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<string>() : string();
}

int intOr(const Json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return 0;
    return static_cast<int>(std::llround(it->get<double>()));
}

string textOf(const Json& j) {
    return j.is_string() ? j.get<string>() : j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// Message from an error body. The live API always answers with a top-level
// "detail" in one of three shapes:
//   {"detail": "Choice question must have at least one choice: c"}      (400)
//   {"detail": {"error_type": "authentication_error", "message": "..."}} (400, 401)
//   {"detail": [{"loc": ["body", "state"], "msg": "Field required"}]}   (422)
// {"error": ...} / {"message": ...} are accepted too, in case a proxy answers.
string errorMessageFromBody(const Json& j) {
    if (j.is_string()) return clipped(j.get<string>());
    if (!j.is_object()) return clipped(textOf(j));

    auto err = j.find("error");
    if (err != j.end()) {
        if (err->is_string()) return clipped(err->get<string>());
        if (err->is_object()) {
            string m = stringOr(*err, "message");
            if (!m.empty()) return clipped(m);
        }
    }
    string m = stringOr(j, "message");
    if (!m.empty()) return clipped(m);

    auto detail = j.find("detail");
    if (detail != j.end()) {
        if (detail->is_string()) return clipped(detail->get<string>());
        if (detail->is_object()) {
            string msg = stringOr(*detail, "message");
            if (!msg.empty()) {
                string type = stringOr(*detail, "error_type");
                return clipped(type.empty() ? msg : type + ": " + msg);
            }
        }
        if (detail->is_array()) {
            string out;
            for (const auto& d : *detail) {
                if (!out.empty()) out += "; ";
                if (!d.is_object()) { out += textOf(d); continue; }
                auto loc = d.find("loc");
                if (loc != d.end() && loc->is_array()) {
                    string path;
                    for (const auto& p : *loc) {
                        if (!path.empty()) path += ".";
                        path += textOf(p);
                    }
                    if (!path.empty()) out += path + ": ";
                }
                string msg = stringOr(d, "msg");
                out += msg.empty() ? textOf(d) : msg;
            }
            return clipped(out);
        }
        return clipped(textOf(*detail));
    }
    return clipped(textOf(j));
}

QuestionType typeFromString(const string& t) {
    if (t == "noul") return QuestionType::Noul;
    if (t == "choice") return QuestionType::Choice;
    if (t == "score") return QuestionType::Score;
    return QuestionType::Unknown;
}

// Reads every known field whatever the type says, so an unknown future type
// still exposes what it shares with the known ones.
Answer parseAnswer(const Json& a) {
    Answer out;
    if (!a.is_object()) return out;
    out.type = typeFromString(stringOr(a, "type"));
    out.noul = numberOr(a, "noul");
    out.choice = stringOr(a, "choice");
    out.score = numberOr(a, "score");
    out.confidence = numberOr(a, "confidence");

    auto probs = a.find("probabilities");
    if (probs != a.end() && probs->is_object()) {
        for (const auto& [key, p] : probs->items()) {
            if (p.is_number()) out.probabilities[key] = p.get<double>();
        }
    }
    auto legend = a.find("legend");
    if (legend != a.end() && legend->is_object()) {
        for (const auto& [key, level] : legend->items()) {
            char* end = nullptr;
            long index = std::strtol(key.c_str(), &end, 10);
            if (key.empty() || end == nullptr || *end != '\0') continue;   // not an integer key
            out.legend[static_cast<int>(index)] = textOf(level);
        }
    }
    return out;
}

ResponseEventArgs parseResponse(uint64_t id, const TransportResponse& res) {
    ResponseEventArgs out;
    out.requestId = id;
    out.statusCode = res.statusCode;
    if (res.statusCode == 0) {
        out.error = describeStatus(res);
        return out;
    }

    Json j = Json::parse(res.body, nullptr, false);
    bool isJson = !j.is_discarded();
    out.raw = isJson ? j : Json(res.body);

    if (res.statusCode < 200 || res.statusCode >= 300) {
        out.error = describeStatus(res);
        string msg = isJson ? errorMessageFromBody(j) : clipped(trimmed(res.body));
        if (!msg.empty()) out.error += ": " + msg;
        return out;
    }
    if (!isJson || !j.is_object()) {
        out.error = "HTTP " + to_string(res.statusCode) + " but the body is not a JSON object: " +
                    clipped(trimmed(res.body), 120);
        return out;
    }
    auto answers = j.find("answers");
    if (answers == j.end() || !answers->is_object()) {
        out.error = "HTTP " + to_string(res.statusCode) + " but the response has no \"answers\" object";
        return out;
    }

    out.ok = true;
    out.model = stringOr(j, "model");
    for (const auto& [name, a] : answers->items()) {
        out.answers[name] = parseAnswer(a);
    }
    auto usage = j.find("usage");
    if (usage != j.end() && usage->is_object()) {
        out.inputTokens = intOr(*usage, "input_tokens");
        out.outputTokens = intOr(*usage, "output_tokens");
    }
    return out;
}

// The default transport: one HTTPS POST through tcxCurl. Verbose curl output
// is never enabled (it would print the request headers).
TransportResponse curlTransport(const TransportRequest& req) {
    tcx::curl::HttpClient http;
    http.setTimeout((std::max)(1L, static_cast<long>(std::ceil(req.timeoutSeconds))));
    if (!req.apiKey.empty()) http.setBearerToken(req.apiKey);
    auto res = http.postRaw(req.url, req.body, "application/json");
    TransportResponse out;
    out.statusCode = res.statusCode;
    out.body = std::move(res.body);
    out.error = std::move(res.error);
    return out;
}

// One accepted request, with the settings it was queued under.
struct Job {
    uint64_t id = 0;
    TransportRequest http;
    int maxRetries = 3;
    float retryInitial = 0.5f;
    float retryMax = 8.0f;
    Transport transport;   // empty = curlTransport
};

TransportResponse callTransport(const Job& job) {
    try {
        return job.transport ? job.transport(job.http) : curlTransport(job.http);
    } catch (const std::exception& e) {
        TransportResponse r;
        r.error = string("transport threw: ") + e.what();
        return r;
    } catch (...) {
        TransportResponse r;
        r.error = "transport threw an unknown exception";
        return r;
    }
}

} // namespace

// =============================================================================
// Request
// =============================================================================

Request& Request::state(const Json& s) {
    state_ = s;
    return *this;
}

Request& Request::noul(const string& name, const Json& instructions) {
    return question(name, Json{{"type", "noul"}, {"instructions", instructions}});
}

Request& Request::noul(const string& name, const Json& instructions,
                       const Json& whenTrue, const Json& whenFalse) {
    Json q{{"type", "noul"}, {"instructions", instructions}};
    // The wire keys are "true" and "false" (API reference, the JS SDK's
    // NoulQuestion type and the Python SDK's NoulCriteria all agree), even
    // where prose calls them the "yes" and "no" descriptions. A null side is
    // left out.
    Json criteria = Json::object();
    if (!whenTrue.is_null()) criteria["true"] = whenTrue;
    if (!whenFalse.is_null()) criteria["false"] = whenFalse;
    if (!criteria.empty()) q["criteria"] = criteria;
    return question(name, q);
}

Request& Request::choice(const string& name, const Json& instructions, const Json& options) {
    Json criteria;
    if (options.is_array()) {
        criteria = Json::object();
        for (const auto& o : options) {
            if (o.is_string()) {
                criteria[o.get<string>()] = nullptr;
            } else {
                logWarning(kLog) << "choice '" << name << "': skipping a non-string option name";
            }
        }
    } else {
        criteria = options;
    }
    if (!criteria.is_object()) {
        logWarning(kLog) << "choice '" << name
                         << "': options should be an object (option -> description) or an array of names";
    } else if (criteria.empty()) {
        logWarning(kLog) << "choice '" << name << "' has no options";
    } else if (criteria.size() > kMaxChoiceOptions) {
        logWarning(kLog) << "choice '" << name << "' has " << criteria.size()
                         << " options; the API accepts at most " << kMaxChoiceOptions;
    }
    return question(name, Json{{"type", "choice"}, {"instructions", instructions}, {"criteria", criteria}});
}

Request& Request::score(const string& name, const Json& instructions, const Json& levels) {
    if (!levels.is_array()) {
        logWarning(kLog) << "score '" << name << "': levels should be an array (lowest level first)";
    } else if (levels.size() < kMinScoreLevels || levels.size() > kMaxScoreLevels) {
        logWarning(kLog) << "score '" << name << "' has " << levels.size() << " levels; the API expects "
                         << kMinScoreLevels << " to " << kMaxScoreLevels;
    }
    return question(name, Json{{"type", "score"}, {"instructions", instructions}, {"criteria", levels}});
}

Request& Request::question(const string& name, const Json& rawQuestion) {
    if (name.empty()) logWarning(kLog) << "question with an empty name";
    if (!rawQuestion.is_object() || !rawQuestion.contains("type")) {
        logWarning(kLog) << "question '" << name << "' is not an object with a \"type\"";
    }
    if (questions_.contains(name)) logWarning(kLog) << "question '" << name << "' replaced";
    questions_[name] = rawQuestion;
    return *this;
}

Request& Request::model(const string& m) {
    model_ = m;
    return *this;
}

Json Request::toJson(const string& defaultModel) const {
    Json body = Json::object();
    body["state"] = state_;
    body["model"] = model_.empty() ? defaultModel : model_;
    body["questions"] = questions_;
    return body;
}

// =============================================================================
// Client::State - everything the worker touches. Shared-owned, so a worker
// that outlives its Client (destroyed from one of its own listeners) still has
// valid memory; `owner` says whether there is anything left to fire.
// =============================================================================

struct Client::State : std::enable_shared_from_this<Client::State> {
    Mutex mutex;                  // guards everything below
#if !defined(__EMSCRIPTEN__)
    std::condition_variable cv;   // new job, stop, fire slot released
    std::thread worker;
#endif
    Client* owner = nullptr;      // cleared by ~Client while no other thread fires

    // Settings
    string apiKey;
    string model = kDefaultModel;
    float timeoutSeconds = 30;
    int maxRetries = 3;
    float retryInitial = 0.5f;
    float retryMax = 8.0f;
    bool async = kThreadsAvailable;
    Transport transport;

    uint64_t nextId = 1;
    bool stop = false;
    deque<Job> jobs;              // waiting for the worker

    // The fire slot: one thread fires responseEvent at a time (re-entrant for
    // that thread, so a listener may call request() or destroy the client).
    // A condition variable instead of a mutex, so a thread waiting for the
    // slot gives up when the client is destroyed: ~Client can then join the
    // worker even when called from a listener that holds the slot.
    std::thread::id firingThread;
    int firingDepth = 0;

    // Runs the HTTP call with retries. Returns false (and fills nothing) when
    // the client was destroyed during a backoff wait.
    bool perform(const Job& job, ResponseEventArgs& out) {
        TransportResponse res;
        for (int attempt = 0;; ++attempt) {
            res = callTransport(job);
            if (!isRetryable(res.statusCode) || attempt >= job.maxRetries) break;
            float delay = backoffSeconds(attempt, job.retryInitial, job.retryMax);
            logNotice(kLog) << "request #" << job.id << ": "
                            << redactKey(describeStatus(res), job.http.apiKey) << "; retry "
                            << (attempt + 1) << "/" << job.maxRetries << " in "
                            << static_cast<int>(delay * 1000.0f + 0.5f) << " ms";
            if (!sleepUnlessStopped(delay)) return false;
        }
        out = parseResponse(job.id, res);
        out.error = redactKey(out.error, job.http.apiKey);
        if (!out.ok) logWarning(kLog) << "request #" << job.id << " failed: " << out.error;
        return true;
    }

    bool sleepUnlessStopped(float seconds) {
#if !defined(__EMSCRIPTEN__)
        auto ms = chrono::milliseconds(static_cast<long long>(seconds * 1000.0f + 0.5f));
        std::unique_lock<Mutex> lock(mutex);
        return !cv.wait_for(lock, ms, [this] { return stop; });
#else
        (void)seconds;
        return true;
#endif
    }

    // Fires responseEvent on this thread; each listener's Deliver decides
    // where it actually runs. Nothing fires once the client is destroyed.
    void fire(ResponseEventArgs& r) {
        Client* target = nullptr;
        {
            std::unique_lock<Mutex> lock(mutex);
            auto me = std::this_thread::get_id();
#if !defined(__EMSCRIPTEN__)
            cv.wait(lock, [&] { return stop || firingDepth == 0 || firingThread == me; });
#endif
            if (stop || !owner) return;
            firingThread = me;
            ++firingDepth;
            target = owner;
        }
        // `owner` stays valid while this thread holds the slot: ~Client on
        // another thread waits for it (on this thread, the listener destroying
        // the client is the last thing that touches it).
        target->responseEvent.notify(r);
        {
            std::lock_guard<Mutex> lock(mutex);
            if (--firingDepth == 0) firingThread = std::thread::id();
        }
#if !defined(__EMSCRIPTEN__)
        cv.notify_all();
#endif
    }

#if !defined(__EMSCRIPTEN__)
    // Starts the worker on first use. Called with `mutex` held (so it doesn't
    // log); returns an error text, empty on success.
    string ensureWorker() {
        if (worker.joinable()) return "";
        try {
            auto self = shared_from_this();
            worker = std::thread([self] { self->workerLoop(); });
            return "";
        } catch (const std::exception& e) {
            return e.what();
        }
    }

    void workerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<Mutex> lock(mutex);
                cv.wait(lock, [this] { return stop || !jobs.empty(); });
                if (stop) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            ResponseEventArgs out;
            if (!perform(job, out)) return;
            fire(out);
        }
    }
#endif
};

// =============================================================================
// Client
// =============================================================================

Client::Client() : state_(std::make_shared<State>()) {
    state_->owner = this;
    // Record the main thread id if nothing has yet (the app runner normally
    // does): Deliver::Main listeners rely on isMainThread().
    getMainThreadId();
    if (const char* env = std::getenv("TYPESAFE_API_KEY")) {
        state_->apiKey = trimmed(env);
    }
}

Client::Client(string apiKey) : Client() {
    setApiKey(std::move(apiKey));
}

Client::~Client() {
    auto s = state_;
    {
        std::unique_lock<Mutex> lock(s->mutex);
        s->stop = true;
        s->jobs.clear();
#if !defined(__EMSCRIPTEN__)
        s->cv.notify_all();
        // Wait for a listener running on another thread to return. Not when
        // this thread holds the slot: then the destructor runs inside one of
        // this client's own listeners.
        auto me = std::this_thread::get_id();
        s->cv.wait(lock, [&] { return s->firingDepth == 0 || s->firingThread == me; });
#endif
        s->owner = nullptr;
    }
#if !defined(__EMSCRIPTEN__)
    s->cv.notify_all();
    if (s->worker.joinable()) {
        if (s->worker.get_id() == std::this_thread::get_id()) {
            // Destroyed from a listener running on the worker: it can't join
            // itself. It exits on its own (stop is set) and owns `s`.
            s->worker.detach();
        } else {
            s->worker.join();
        }
    }
#endif
}

Client& Client::setApiKey(string apiKey) {
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->apiKey = trimmed(apiKey);
    return *this;
}

bool Client::hasApiKey() const {
    std::lock_guard<Mutex> lock(state_->mutex);
    return !state_->apiKey.empty();
}

Client& Client::setModel(string model) {
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->model = model.empty() ? string(kDefaultModel) : std::move(model);
    return *this;
}

string Client::getModel() const {
    std::lock_guard<Mutex> lock(state_->mutex);
    return state_->model;
}

Client& Client::setTimeout(float seconds) {
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->timeoutSeconds = (std::max)(seconds, 0.001f);
    return *this;
}

float Client::getTimeout() const {
    std::lock_guard<Mutex> lock(state_->mutex);
    return state_->timeoutSeconds;
}

Client& Client::setMaxRetries(int retries) {
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->maxRetries = (std::max)(retries, 0);
    return *this;
}

int Client::getMaxRetries() const {
    std::lock_guard<Mutex> lock(state_->mutex);
    return state_->maxRetries;
}

Client& Client::setRetryDelay(float initialSeconds, float maxSeconds) {
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->retryInitial = (std::max)(initialSeconds, 0.0f);
    state_->retryMax = (std::max)(maxSeconds, state_->retryInitial);
    return *this;
}

Client& Client::setTransport(Transport transport) {
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->transport = std::move(transport);
    return *this;
}

bool Client::isAsyncSupported() {
    return kThreadsAvailable;
}

Client& Client::setAsync(bool async) {
    bool effective = detail::resolveAsync(async, isAsyncSupported());
    if (async && !effective) {
        logError(kLog) << "setAsync(true): worker threads are not available on this platform; async stays off";
    }
    std::lock_guard<Mutex> lock(state_->mutex);
    state_->async = effective;
    return *this;
}

bool Client::isAsync() const {
    std::lock_guard<Mutex> lock(state_->mutex);
    return state_->async;
}

uint64_t Client::request(const Json& state, const Json& questions) {
    if (!questions.is_object()) {
        logError(kLog) << "request(): questions must be a JSON object (name -> question)";
        return 0;
    }
    Request req;
    req.state(state);
    for (const auto& [name, q] : questions.items()) req.question(name, q);
    return request(req);
}

uint64_t Client::request(const Request& req) {
#if defined(__EMSCRIPTEN__)
    (void)req;
    logError(kLog) << "request(): not available in web builds (no worker threads, and tcxCurl has no web backend)";
    return 0;
#else
    if (!req.getQuestions().is_object() || req.getQuestions().empty()) {
        logError(kLog) << "request(): the request has no questions";
        return 0;
    }

    auto s = state_;   // a listener fired below (sync mode) may destroy this client
    bool canSend;
    string model;
    {
        std::lock_guard<Mutex> lock(s->mutex);
        canSend = !s->apiKey.empty() || s->transport;
        model = s->model;
    }
    if (!canSend) {
        logError(kLog) << "request(): no API key. Set the TYPESAFE_API_KEY environment variable or call setApiKey()";
        return 0;
    }

    // Build the body outside the lock: the state can be large.
    Job job;
    job.http.url = kEndpoint;
    job.http.body = req.toJson(model).dump(-1, ' ', false, Json::error_handler_t::replace);

    bool async;
    string workerError;
    {
        std::lock_guard<Mutex> lock(s->mutex);
        job.id = s->nextId++;
        job.http.apiKey = s->apiKey;
        job.http.timeoutSeconds = s->timeoutSeconds;
        job.maxRetries = s->maxRetries;
        job.retryInitial = s->retryInitial;
        job.retryMax = s->retryMax;
        job.transport = s->transport;
        async = s->async;
        if (async) {
            workerError = s->ensureWorker();
            if (workerError.empty()) s->jobs.push_back(job);
        }
    }
    if (!workerError.empty()) {
        logError(kLog) << "request(): could not start the worker thread: " << workerError;
        return 0;
    }
    uint64_t id = job.id;
    if (async) {
        s->cv.notify_all();
        return id;
    }

    // Sync: HTTP and responseEvent on this thread, before returning.
    ResponseEventArgs out;
    if (s->perform(job, out)) s->fire(out);
    return id;
#endif
}

} // namespace tcx::jev
