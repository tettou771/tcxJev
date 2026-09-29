// =============================================================================
// tcxJev tests - headless, network-free behavioral test (no window).
//
// Built and run by CI on every push/PR across macOS / Windows / Linux (exit 0 =
// pass, non-zero = fail). Every HTTP call goes through Client::setTransport()
// to a scripted fake server, so no API key or network is needed.
//
// Covers: request JSON for all question types + the raw form, response parsing,
// errors and retries, request ids, async FIFO, main-thread delivery vs
// setResponseEventAsync (incl. the flush-on-switch rule), sync mode, client
// destruction with work pending, the async decision function, and that the API
// key never reaches a log line.
// =============================================================================

#include <TrussC.h>
#include <tcxJev.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace tc;
using namespace tcx::jev;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;
static void check(const string& name, bool ok) {
    std::printf("%-72s %s\n", name.c_str(), ok ? "PASS" : "FAIL");
    std::fflush(stdout);   // flush per line so CI logs survive a later crash
    ok ? ++g_pass : ++g_fail;
}

static void sleepMs(int ms) { this_thread::sleep_for(chrono::milliseconds(ms)); }

// A fake key that must never appear in any log line.
static const string kKey = "tsk-test-7f3c9a0d51e24b86-SECRET";
static const string kEnvKey = "tsk-env-5b1d0c9e77a24f13-SECRET";

static void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// One simulated frame start: run what was queued with runOnMainThread.
static atomic<bool> g_inDrain{false};
static void drainFrame() {
    g_inDrain = true;
    internal::drainMainThreadQueue();
    g_inDrain = false;
}

template <class F>
static bool waitFor(F cond, int timeoutMs = 5000, bool drain = false) {
    auto end = chrono::steady_clock::now() + chrono::milliseconds(timeoutMs);
    while (!cond()) {
        if (drain) drainFrame();
        if (chrono::steady_clock::now() > end) return false;
        sleepMs(1);
    }
    return true;
}

// Captures every log line (tests assert on errors, and that no key leaks).
struct LogCapture {
    mutex m;
    vector<pair<LogLevel, string>> lines;
    EventListener listener;
    LogCapture() {
        listener = getLogger().onLog.listen([this](LogEventArgs& e) {
            lock_guard<mutex> lock(m);
            lines.push_back({e.level, e.message});
        });
    }
    size_t size() {
        lock_guard<mutex> lock(m);
        return lines.size();
    }
    bool errorSince(size_t from, const string& needle) {
        lock_guard<mutex> lock(m);
        for (size_t i = from; i < lines.size(); ++i) {
            if (lines[i].first == LogLevel::Error && lines[i].second.find(needle) != string::npos) return true;
        }
        return false;
    }
    bool anyContains(const string& needle) {
        lock_guard<mutex> lock(m);
        for (auto& l : lines) {
            if (l.second.find(needle) != string::npos) return true;
        }
        return false;
    }
};

// Blocks a transport call until released (bounded, so a bug can't hang CI).
struct Gate {
    mutex m;
    condition_variable cv;
    bool open = false;
    atomic<bool> reached{false};
    void wait() {
        reached = true;
        unique_lock<mutex> lock(m);
        cv.wait_for(lock, chrono::seconds(10), [this] { return open; });
    }
    void release() {
        {
            lock_guard<mutex> lock(m);
            open = true;
        }
        cv.notify_all();
    }
};

static TransportResponse reply(int status, const string& body, const string& error = "") {
    TransportResponse r;
    r.statusCode = status;
    r.body = body;
    r.error = error;
    return r;
}

static const string kNoulBody =
    R"({"model":"jev-1.13.0","answers":{"q":{"type":"noul","noul":0.75}},"usage":{"input_tokens":10,"output_tokens":2}})";

// Scripted fake server: answers with the queued replies in order, then with
// `fallback`. Records every request and the thread it arrived on.
struct FakeServer {
    mutex m;
    deque<TransportResponse> script;
    TransportResponse fallback = reply(200, kNoulBody);
    vector<TransportRequest> requests;
    vector<thread::id> threads;
    atomic<int> calls{0};
    atomic<int> done{0};
    atomic<int> inFlight{0};
    atomic<int> maxInFlight{0};
    function<void(int)> hook;   // called with the 0-based call index

    void push(TransportResponse r) {
        lock_guard<mutex> lock(m);
        script.push_back(std::move(r));
    }
    Transport transport() {
        return [this](const TransportRequest& req) { return handle(req); };
    }
    TransportResponse handle(const TransportRequest& req) {
        int index = calls.fetch_add(1);
        int now = ++inFlight;
        int prev = maxInFlight.load();
        while (now > prev && !maxInFlight.compare_exchange_weak(prev, now)) {}
        {
            lock_guard<mutex> lock(m);
            requests.push_back(req);
            threads.push_back(this_thread::get_id());
        }
        if (hook) hook(index);
        TransportResponse res;
        {
            lock_guard<mutex> lock(m);
            if (!script.empty()) {
                res = script.front();
                script.pop_front();
            } else {
                res = fallback;
            }
        }
        --inFlight;
        ++done;
        return res;
    }
    TransportRequest request(size_t i) {
        lock_guard<mutex> lock(m);
        return i < requests.size() ? requests[i] : TransportRequest{};
    }
    Json body(size_t i) { return Json::parse(request(i).body, nullptr, false); }
};

// Collects responseEvent payloads with the thread each one fired on.
struct Collector {
    mutex m;
    vector<ResponseEventArgs> events;
    vector<thread::id> threads;
    vector<bool> onMain;
    vector<bool> duringDrain;
    atomic<int> count{0};
    EventListener listener;
    function<void(ResponseEventArgs&)> extra;

    void attach(Client& c) {
        listener = c.responseEvent.listen([this](ResponseEventArgs& e) {
            {
                lock_guard<mutex> lock(m);
                events.push_back(e);
                threads.push_back(this_thread::get_id());
                onMain.push_back(isMainThread());
                duringDrain.push_back(g_inDrain.load());
            }
            ++count;
            if (extra) extra(e);
        });
    }
    ResponseEventArgs at(size_t i) {
        lock_guard<mutex> lock(m);
        return i < events.size() ? events[i] : ResponseEventArgs{};
    }
    vector<uint64_t> ids() {
        lock_guard<mutex> lock(m);
        vector<uint64_t> out;
        for (auto& e : events) out.push_back(e.requestId);
        return out;
    }
};

// Sync client wired to a fake server, with near-instant retries.
static void useFake(Client& c, FakeServer& server) {
    c.setApiKey(kKey)
        .setTransport(server.transport())
        .setAsync(false)
        .setRetryDelay(0.001f, 0.004f);
}

static Request simpleRequest(int n = 0) {
    return Request().state("state " + to_string(n)).noul("q", "Is it?");
}

// One sync request fired on the main thread; returns what the event carried.
static ResponseEventArgs roundTrip(Client& c, const Request& req, bool* firedBeforeReturn = nullptr) {
    Collector col;
    col.attach(c);
    c.request(req);
    if (firedBeforeReturn) *firedBeforeReturn = col.count == 1;
    return col.at(0);
}

// ---------------------------------------------------------------------------
// A. Request JSON
// ---------------------------------------------------------------------------

static void testRequestJson() {
    {
        Json got = Request().state("hello").noul("q", "Is it?").toJson();
        Json want = Json::parse(R"({"state":"hello","model":"jev-latest",
            "questions":{"q":{"type":"noul","instructions":"Is it?"}}})");
        check("json: noul without criteria", got == want);
    }
    {
        Json got = Request().state("s").noul("q", "Is it?", "Clearly yes", "Clearly no").toJson();
        Json want = Json::parse(R"({"type":"noul","instructions":"Is it?",
            "criteria":{"true":"Clearly yes","false":"Clearly no"}})");
        check("json: noul criteria use \"true\"/\"false\" keys", got["questions"]["q"] == want);

        Json oneSide = Request().noul("q", "Is it?", "Clearly yes", nullptr).toJson();
        check("json: noul criteria leaves out a null side",
              oneSide["questions"]["q"]["criteria"] == Json::parse(R"({"true":"Clearly yes"})"));
    }
    {
        Json got = Request().state("s").choice("dept", "Which team?", {
            {"billing", "Payments, invoicing, refunds"},
            {"technical", nullptr},
            {"sales", {{"what", "Pricing"}, {"examples", {"upgrade", "quote"}}}},
        }).toJson();
        Json want = Json::parse(R"({"type":"choice","instructions":"Which team?","criteria":{
            "billing":"Payments, invoicing, refunds","technical":null,
            "sales":{"what":"Pricing","examples":["upgrade","quote"]}}})");
        check("json: choice with described / null / structured options", got["questions"]["dept"] == want);

        Json fromArray = Request().choice("color", "Which color?", {"red", "green", "blue"}).toJson();
        check("json: choice from an array of names -> null descriptions",
              fromArray["questions"]["color"]["criteria"] ==
                  Json::parse(R"({"red":null,"green":null,"blue":null})"));

        Json two = Request().choice("yn", "Pick", {"left", "right"}).toJson();
        check("json: two-name choice list stays a list of options",
              two["questions"]["yn"]["criteria"] == Json::parse(R"({"left":null,"right":null})"));
    }
    {
        Json got = Request().state("s").score("f", "How frustrated?", {"Calm", "Frustrated", "Very angry"}).toJson();
        Json want = Json::parse(R"({"type":"score","instructions":"How frustrated?",
            "criteria":["Calm","Frustrated","Very angry"]})");
        check("json: score levels in order", got["questions"]["f"] == want);

        vector<string> levels = {"Low", "High"};
        Json fromVector = Request().score("v", "Level?", levels).toJson();
        check("json: score levels from a vector<string>",
              fromVector["questions"]["v"]["criteria"] == Json::parse(R"(["Low","High"])"));
    }
    {
        Json raw = Json::parse(R"({"type":"noul","instructions":{"question":"Same person as `p`?","p":{"name":"A"}}})");
        Json state = Json::parse(R"({"name":"A","city":"Oakland"})");
        Json got = Request().state(state).question("same", raw).model("jev-1.13.0").toJson("ignored");
        check("json: raw question + object state + per-request model",
              got["questions"]["same"] == raw && got["state"] == state && got["model"] == "jev-1.13.0");
    }

    // What the client actually sends.
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        c.setModel("jev-1.13.0").setTimeout(12.5f);
        c.request(Request().state("hello").noul("q", "Is it?"));
        TransportRequest sent = server.request(0);
        check("send: POST url is the systemone endpoint", sent.url == "https://api.typesafe.ai/v1/systemone");
        check("send: api key handed to the transport", sent.apiKey == kKey);
        check("send: timeout handed to the transport", sent.timeoutSeconds == 12.5f);
        check("send: body uses the client's model", server.body(0)["model"] == "jev-1.13.0");
        check("send: body carries state and questions",
              server.body(0)["state"] == "hello" && server.body(0)["questions"]["q"]["type"] == "noul");

        c.request(Request().state("x").noul("q", "Is it?").model("jev-preview"));
        check("send: per-request model beats the client's", server.body(1)["model"] == "jev-preview");

        c.setModel("");
        c.request(simpleRequest());
        check("send: setModel(\"\") restores jev-latest", server.body(2)["model"] == "jev-latest");
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        Json questions = Json::parse(R"({"a":{"type":"noul","instructions":"A?"},
            "b":{"type":"score","instructions":"B?","criteria":["lo","hi"]}})");
        uint64_t id = c.request(Json::parse(R"(["line 1","line 2"])"), questions);
        Json body = server.body(0);
        check("raw form: accepted", id > 0);
        check("raw form: state and questions sent as given",
              body["state"] == Json::parse(R"(["line 1","line 2"])") && body["questions"] == questions &&
                  body["model"] == "jev-latest");
    }
}

// ---------------------------------------------------------------------------
// B. Response parsing
// ---------------------------------------------------------------------------

static void testParsing() {
    FakeServer server;
    Client c;
    useFake(c, server);

    server.push(reply(200, R"({
        "model": "jev-1.13.0",
        "answers": {
            "urgent": {"type": "noul", "noul": 0.95},
            "department": {"type": "choice", "choice": "billing",
                           "probabilities": {"billing": 0.88, "technical": 0.12, "sales": 0.0},
                           "confidence": 0.81},
            "frustration": {"type": "score", "score": 1.05,
                            "legend": {"0": "Calm", "1": "Frustrated", "2": "Very angry"},
                            "probabilities": {"0": 0.0, "1": 0.95, "2": 0.05},
                            "confidence": 0.92}
        },
        "usage": {"input_tokens": 318, "output_tokens": 34}
    })"));
    ResponseEventArgs e = roundTrip(c, simpleRequest());
    check("parse: ok, status, model, usage",
          e.ok && e.statusCode == 200 && e.error.empty() && e.model == "jev-1.13.0" &&
              e.inputTokens == 318 && e.outputTokens == 34);
    Answer n = e.answers["urgent"];
    check("parse: noul answer", n.type == QuestionType::Noul && n.noul == 0.95 && n.confidence == 0);
    Answer ch = e.answers["department"];
    check("parse: choice answer", ch.type == QuestionType::Choice && ch.choice == "billing" &&
                                      ch.confidence == 0.81 && ch.probabilities.size() == 3 &&
                                      ch.probabilities["technical"] == 0.12);
    Answer s = e.answers["frustration"];
    check("parse: score answer", s.type == QuestionType::Score && s.score == 1.05 && s.confidence == 0.92 &&
                                     s.legend.size() == 3 && s.legend[0] == "Calm" &&
                                     s.legend[2] == "Very angry" && s.probabilities["1"] == 0.95);
    check("parse: raw keeps the full body", e.raw["answers"]["frustration"]["legend"]["1"] == "Frustrated");

    server.push(reply(200, R"({"answers":{"a":{"type":"noul"},"b":{"type":"choice"},"c":{"type":"score"}}})"));
    e = roundTrip(c, simpleRequest());
    check("parse: missing fields default (no model/usage/values)",
          e.ok && e.model.empty() && e.inputTokens == 0 && e.outputTokens == 0 &&
              e.answers["a"].type == QuestionType::Noul && e.answers["a"].noul == 0 &&
              e.answers["b"].choice.empty() && e.answers["b"].probabilities.empty() &&
              e.answers["c"].score == 0 && e.answers["c"].legend.empty());

    server.push(reply(200, R"({"answers":{"x":{"type":"ranking","score":2.5,"confidence":0.5},
                                          "y":{"noul":0.3},"z":5}})"));
    e = roundTrip(c, simpleRequest());
    check("parse: unknown type -> Unknown, shared fields still read",
          e.ok && e.answers["x"].type == QuestionType::Unknown && e.answers["x"].score == 2.5 &&
              e.answers["x"].confidence == 0.5);
    check("parse: answer without type / non-object answer -> Unknown",
          e.answers["y"].type == QuestionType::Unknown && e.answers["y"].noul == 0.3 &&
              e.answers["z"].type == QuestionType::Unknown);

    server.push(reply(200, R"({"answers":{"s":{"type":"score","score":0.5,
        "legend":{"0":{"what":"low"},"1":"high","top":"skip me"},"probabilities":{"0":"bad","1":0.5}},
        "n":{"type":"noul","noul":"high"}},"usage":{"input_tokens":"many","output_tokens":3.0}})"));
    e = roundTrip(c, simpleRequest());
    check("parse: structured legend kept as JSON text, non-integer key skipped",
          e.ok && e.answers["s"].legend.size() == 2 && e.answers["s"].legend[1] == "high" &&
              Json::parse(e.answers["s"].legend[0], nullptr, false) == Json::parse(R"({"what":"low"})"));
    check("parse: wrong-typed values ignored without throwing",
          e.answers["s"].probabilities.size() == 1 && e.answers["n"].noul == 0 && e.inputTokens == 0 &&
              e.outputTokens == 3);

    server.push(reply(200, R"({"model":"jev-1.13.0"})"));
    e = roundTrip(c, simpleRequest());
    check("parse: 200 without answers -> not ok",
          !e.ok && e.statusCode == 200 && e.error.find("answers") != string::npos);

    server.push(reply(200, "<html>proxy page</html>"));
    e = roundTrip(c, simpleRequest());
    check("parse: 200 with a non-JSON body -> not ok, raw is the text",
          !e.ok && e.raw.is_string() && e.raw.get<string>() == "<html>proxy page</html>" &&
              e.error.find("not a JSON object") != string::npos);
}

// ---------------------------------------------------------------------------
// C. Errors and retries
// ---------------------------------------------------------------------------

static void testErrors() {
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        server.push(reply(401, R"({"detail":"Invalid API key"})"));
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("401: not retried", server.calls == 1);
        check("401: not ok, status and message",
              !e.ok && e.statusCode == 401 && e.error.find("401") != string::npos &&
                  e.error.find("Invalid API key") != string::npos && e.raw["detail"] == "Invalid API key");
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        server.push(reply(422, R"({"detail":[{"loc":["body","questions","q","criteria"],
            "msg":"Field required","type":"missing"},{"loc":["body","model"],"msg":"Unknown model"}]})"));
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("422: not retried, validation details in the error",
              server.calls == 1 && !e.ok && e.statusCode == 422 &&
                  e.error.find("body.questions.q.criteria: Field required") != string::npos &&
                  e.error.find("body.model: Unknown model") != string::npos);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        server.push(reply(429, R"({"error":{"message":"rate limited"}})"));
        server.push(reply(429, R"({"error":"slow down"})"));
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("429 x2 then 200: retried to success", server.calls == 3 && e.ok && e.statusCode == 200 &&
                                                         e.answers["q"].noul == 0.75);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        server.fallback = reply(529, R"({"message":"overloaded"})");
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("529 always: 1 + 3 retries, then fails",
              server.calls == 4 && !e.ok && e.statusCode == 529 && e.error.find("Overloaded") != string::npos &&
                  e.error.find("overloaded") != string::npos);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        c.setMaxRetries(1);
        server.push(reply(500, "oops"));
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("500 then 200: 5xx retried", server.calls == 2 && e.ok);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        server.fallback = reply(0, "", "Couldn't connect to server");
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("network error: retried, then reported",
              server.calls == 4 && !e.ok && e.statusCode == 0 && e.error.find("network error") != string::npos &&
                  e.error.find("Couldn't connect") != string::npos);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        c.setMaxRetries(0);
        server.push(reply(502, "<html><body>Bad gateway</body></html>"));
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("setMaxRetries(0): single attempt", server.calls == 1);
        check("non-JSON error body: text in the error, raw is a string",
              !e.ok && e.statusCode == 502 && e.error.find("Bad gateway") != string::npos && e.raw.is_string());
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        c.setMaxRetries(0);
        c.setTransport([](const TransportRequest&) -> TransportResponse { throw runtime_error("boom"); });
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("throwing transport: reported, not propagated",
              !e.ok && e.error.find("transport threw: boom") != string::npos);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        server.push(reply(401, R"({"detail":"Invalid API key )" + kKey + R"("})"));
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        check("error text never carries the key (echoed by the server)",
              e.error.find(kKey) == string::npos && e.error.find("<redacted>") != string::npos);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        c.setRetryDelay(0.08f, 0.08f);
        server.push(reply(429, "{}"));
        auto t0 = chrono::steady_clock::now();
        ResponseEventArgs e = roundTrip(c, simpleRequest());
        double waited = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
        check("backoff: waits before retrying (>= delay minus 25% jitter)", e.ok && waited >= 0.055);
    }
}

// ---------------------------------------------------------------------------
// D. Ids, rejected requests, API key sources
// ---------------------------------------------------------------------------

static void testIds(LogCapture& logs) {
    FakeServer server;
    Client c;
    useFake(c, server);
    Collector col;
    col.attach(c);
    uint64_t a = c.request(simpleRequest(1));
    uint64_t b = c.request(simpleRequest(2));
    size_t mark = logs.size();
    uint64_t empty = c.request(Request().state("no questions"));
    uint64_t badRaw = c.request(Json("s"), Json::array({1, 2}));
    uint64_t d = c.request(simpleRequest(3));
    check("ids: start at 1 and increase", a == 1 && b == 2 && d == 3);
    check("ids: each response carries its request's id", col.ids() == vector<uint64_t>({1, 2, 3}));
    check("rejected: no questions -> 0 + error log", empty == 0 && logs.errorSince(mark, "no questions"));
    check("rejected: raw questions not an object -> 0 + error log",
          badRaw == 0 && logs.errorSince(mark, "questions must be a JSON object"));
    check("rejected requests fire nothing", col.count == 3);

    {
        Client noKey;   // env var cleared in main()
        mark = logs.size();
        uint64_t id = noKey.request(simpleRequest());
        check("rejected: no API key and default transport -> 0 + error log",
              !noKey.hasApiKey() && id == 0 && logs.errorSince(mark, "no API key"));
    }
    {
        FakeServer s2;
        Client viaProxy;
        viaProxy.setTransport(s2.transport()).setAsync(false);
        check("custom transport: no API key needed", viaProxy.request(simpleRequest()) == 1 && s2.calls == 1);
    }
    {
        setEnv("TYPESAFE_API_KEY", ("  " + kEnvKey + "\n").c_str());
        FakeServer s3;
        Client fromEnv;
        fromEnv.setTransport(s3.transport()).setAsync(false);
        fromEnv.request(simpleRequest());
        check("TYPESAFE_API_KEY: read and trimmed", fromEnv.hasApiKey() && s3.request(0).apiKey == kEnvKey);
        Client explicitKey(kKey);
        explicitKey.setTransport(s3.transport()).setAsync(false);
        explicitKey.request(simpleRequest());
        check("Client(key): explicit key wins over the env var", s3.request(1).apiKey == kKey);
        setEnv("TYPESAFE_API_KEY", nullptr);
    }
}

// ---------------------------------------------------------------------------
// E. Async: FIFO on one worker
// ---------------------------------------------------------------------------

static void testAsyncFifo() {
    FakeServer server;
    server.hook = [](int i) { sleepMs(1 + (7 - i % 7)); };   // uneven latencies
    Client c;
    c.setApiKey(kKey).setTransport(server.transport()).setResponseEventAsync(true);
    Collector col;
    col.attach(c);
    const int N = 10;
    vector<uint64_t> ids;
    for (int i = 0; i < N; ++i) ids.push_back(c.request(simpleRequest(i)));
    check("async: request() returns before the HTTP call", server.done < N);
    bool all = waitFor([&] { return col.count == N; });
    check("async: every request answered", all);
    check("async: responses in request order", col.ids() == ids);
    check("async: one HTTP call at a time", server.maxInFlight == 1);
    bool bodiesInOrder = true;
    for (int i = 0; i < N; ++i) bodiesInOrder &= server.body(i)["state"] == "state " + to_string(i);
    check("async: sent in FIFO order", bodiesInOrder);
    bool offMain = true;
    for (int i = 0; i < N; ++i) offMain &= server.request(i).url.size() > 0 && server.threads[i] != this_thread::get_id();
    check("async: HTTP runs off the main thread", offMain);
    bool eventsOnWorker = true;
    for (int i = 0; i < N; ++i) eventsOnWorker &= !col.onMain[i];
    check("async + eventAsync: fired on the worker, no frame needed", eventsOnWorker);
}

// ---------------------------------------------------------------------------
// F. Main-thread delivery (the default) and G. flush on switch
// ---------------------------------------------------------------------------

static void testMainThreadDelivery() {
    FakeServer server;
    Gate gate;
    server.hook = [&](int i) { if (i == 3) gate.wait(); };
    Client c;
    c.setApiKey(kKey).setTransport(server.transport());
    check("defaults: async on, main-thread delivery", c.isAsync() && !c.isResponseEventAsync());
    Collector col;
    col.attach(c);
    for (int i = 0; i < 4; ++i) c.request(simpleRequest(i));
    // The worker is FIFO: once call #4 has started, responses 1..3 are queued.
    bool reached = waitFor([&] { return gate.reached.load(); });
    sleepMs(20);
    check("main delivery: nothing fires without a frame", reached && col.count == 0);
    drainFrame();
    check("main delivery: queued responses fire at the frame start", col.count == 3);
    check("main delivery: in request order", col.ids() == vector<uint64_t>({1, 2, 3}));
    bool main = true;
    for (int i = 0; i < 3; ++i) main &= col.onMain[i] && col.duringDrain[i];
    check("main delivery: on the main thread, inside the drain", main);
    gate.release();
    bool fourth = waitFor([&] { return col.count == 4; }, 5000, true);
    check("main delivery: later response fires on a later frame", fourth && col.onMain[3] && col.duringDrain[3]);
}

static void testFlushOnSwitch() {
    FakeServer server;
    Gate gate;
    server.hook = [&](int i) { if (i == 3) gate.wait(); };
    Client c;
    c.setApiKey(kKey).setTransport(server.transport());
    Collector col;
    col.attach(c);
    for (int i = 0; i < 4; ++i) c.request(simpleRequest(i));
    bool reached = waitFor([&] { return gate.reached.load(); });
    sleepMs(20);
    check("switch: 3 responses queued for the main thread", reached && col.count == 0);

    c.setResponseEventAsync(true);   // must flush 1..3 right here, in order
    check("switch to async: queued responses fired before it returns", col.count == 3);
    check("switch to async: in order", col.ids() == vector<uint64_t>({1, 2, 3}));
    check("switch to async: flushed on the calling thread (main), not in a drain",
          col.onMain[0] && col.onMain[2] && !col.duringDrain[0]);

    gate.release();
    bool fourth = waitFor([&] { return col.count == 4; });   // no drainFrame(): fires immediately
    check("after switch: next response fires immediately on the worker", fourth && !col.onMain[3]);
    drainFrame();
    check("after switch: the stale main-thread drain fires nothing twice", col.count == 4);

    c.setResponseEventAsync(false);
    c.request(simpleRequest(5));
    bool sent = waitFor([&] { return server.done == 5; });
    sleepMs(30);
    check("switch back: waits for a frame again", sent && col.count == 4);
    bool fifth = waitFor([&] { return col.count == 5; }, 5000, true);
    check("switch back: fires on the main thread", fifth && col.onMain[4] && col.duringDrain[4]);
}

// ---------------------------------------------------------------------------
// H. Sync mode
// ---------------------------------------------------------------------------

static void testSync() {
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        bool firedBeforeReturn = false;
        ResponseEventArgs e = roundTrip(c, simpleRequest(), &firedBeforeReturn);
        check("sync: fires before request() returns", firedBeforeReturn && e.ok && e.requestId == 1);
        check("sync: HTTP ran on the calling thread", server.threads[0] == this_thread::get_id());
    }
    {
        // From another thread with main-thread delivery: queued for the main thread.
        FakeServer server;
        Client c;
        useFake(c, server);
        Collector col;
        col.attach(c);
        int countAtReturn = -1;
        thread t([&] {
            c.request(simpleRequest());
            countAtReturn = col.count;
        });
        t.join();
        check("sync off-main + main delivery: not fired on that thread", countAtReturn == 0 && col.count == 0);
        drainFrame();
        check("sync off-main + main delivery: fires on the main thread next frame",
              col.count == 1 && col.onMain[0]);
    }
    {
        FakeServer server;
        Client c;
        useFake(c, server);
        c.setResponseEventAsync(true);
        Collector col;
        col.attach(c);
        int countAtReturn = -1;
        thread::id tid;
        thread t([&] {
            tid = this_thread::get_id();
            c.request(simpleRequest());
            countAtReturn = col.count;
        });
        t.join();
        check("sync off-main + eventAsync: fired on the calling thread before return",
              countAtReturn == 1 && col.threads[0] == tid);
    }
}

// ---------------------------------------------------------------------------
// I. Destruction with work pending
// ---------------------------------------------------------------------------

static void testDestruction() {
    {
        FakeServer server;
        server.hook = [](int) { sleepMs(150); };
        Collector col;
        auto t0 = chrono::steady_clock::now();
        {
            Client c;
            c.setApiKey(kKey).setTransport(server.transport());
            col.attach(c);
            for (int i = 0; i < 3; ++i) c.request(simpleRequest(i));
            waitFor([&] { return server.calls == 1; });
        }   // destroyed with one call in flight and two queued
        double took = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
        drainFrame();
        sleepMs(50);
        drainFrame();
        check("destroy: in-flight + queued requests fire nothing", col.count == 0);
        check("destroy: queued requests dropped (only the in-flight call ran)", server.calls == 1);
        check("destroy: does not hang (waits only for the in-flight call)", took < 3.0);
    }
    {
        FakeServer server;
        Collector col;
        {
            Client c;
            c.setApiKey(kKey).setTransport(server.transport());
            col.attach(c);
            c.request(simpleRequest(1));
            c.request(simpleRequest(2));
            waitFor([&] { return server.done == 2; });
            sleepMs(50);   // responses now wait for the main thread
        }
        drainFrame();
        check("destroy: responses queued for the main thread never fire", col.count == 0);
    }
    {
        // Destroyed by its own listener during a main-thread drain.
        FakeServer server;
        auto c = make_unique<Client>();
        c->setApiKey(kKey).setTransport(server.transport());
        Collector col;
        col.attach(*c);
        col.extra = [&](ResponseEventArgs&) { c.reset(); };
        for (int i = 0; i < 3; ++i) c->request(simpleRequest(i));
        waitFor([&] { return server.done == 3; });
        sleepMs(50);
        drainFrame();
        drainFrame();
        check("destroy from a main-thread listener: no crash, rest dropped", col.count == 1 && !c);
    }
    {
        // Destroyed by its own listener running on the worker thread. The first
        // call waits until all three are queued, so the main thread is done
        // with `c` before the listener resets it.
        FakeServer server;
        Gate gate;
        server.hook = [&](int i) { if (i == 0) gate.wait(); };
        auto c = make_unique<Client>();
        c->setApiKey(kKey).setTransport(server.transport()).setResponseEventAsync(true);
        Collector col;
        col.attach(*c);
        atomic<bool> destroyed{false};
        col.extra = [&](ResponseEventArgs&) {
            c.reset();
            destroyed = true;
        };
        for (int i = 0; i < 3; ++i) c->request(simpleRequest(i));
        gate.release();
        bool gone = waitFor([&] { return destroyed.load(); });
        sleepMs(100);
        drainFrame();
        check("destroy from a worker listener: no crash, rest dropped", gone && col.count == 1);
    }
    {
        Client unused;   // never started a worker
        check("destroy: idle client", true);
    }
}

// ---------------------------------------------------------------------------
// J. Async decision (the web path cannot run here, so test the rule itself)
// ---------------------------------------------------------------------------

static void testAsyncDecision(LogCapture& logs) {
    static_assert(!detail::resolveAsync(true, false), "no threads -> async off");
    static_assert(detail::resolveAsync(true, true), "threads -> async on");
    static_assert(!detail::resolveAsync(false, true), "async off when not requested");
    static_assert(!detail::resolveAsync(false, false), "async off when not requested");
    check("resolveAsync: true only when requested and threads exist", true);
    check("isAsyncSupported: true on desktop", Client::isAsyncSupported());

    Client c;
    size_t mark = logs.size();
    c.setAsync(false);
    check("setAsync(false)", !c.isAsync());
    c.setAsync(true);
    check("setAsync(true) where threads exist: on, no error", c.isAsync() && !logs.errorSince(mark, "setAsync"));
}

// ---------------------------------------------------------------------------
// K. A real headless frame loop: delivery happens in the frame drain
// ---------------------------------------------------------------------------

static atomic<int> g_appFired{0};
static atomic<bool> g_appFiredOnMain{false};
static atomic<bool> g_appSawInUpdate{false};
static atomic<bool> g_appTimedOut{false};

struct DeliveryApp : App {
    FakeServer server;
    Client jev;
    EventListener listener;
    int frames = 0;
    int firedAtFrame = -1;

    void setup() override {
        server.hook = [](int) { sleepMs(30); };
        jev.setApiKey(kKey).setTransport(server.transport());
        listener = jev.responseEvent.listen([this](ResponseEventArgs& e) {
            ++g_appFired;
            g_appFiredOnMain = isMainThread() && e.ok;
            firedAtFrame = frames;
        });
        jev.request(simpleRequest());
    }
    void update() override {
        ++frames;
        if (firedAtFrame >= 0 && frames > firedAtFrame) g_appSawInUpdate = true;
        if (firedAtFrame >= 0 && frames > firedAtFrame + 2) requestExit();
        if (frames > 2000) {
            g_appTimedOut = true;
            requestExit();
        }
    }
};

static void testHeadlessApp() {
    HeadlessSettings hs;
    hs.setFps(500.0f);
    runHeadlessApp<DeliveryApp>(hs);
    check("frame loop: response delivered once, on the main thread",
          !g_appTimedOut && g_appFired == 1 && g_appFiredOnMain);
    check("frame loop: the next update() sees it", g_appSawInUpdate.load());
}

// ---------------------------------------------------------------------------

int main() {
    getMainThreadId();                       // record the main thread (no app runner yet)
    setEnv("TYPESAFE_API_KEY", nullptr);     // tests must not depend on the environment
    LogCapture logs;

    testRequestJson();
    testParsing();
    testErrors();
    testIds(logs);
    testAsyncFifo();
    testMainThreadDelivery();
    testFlushOnSwitch();
    testSync();
    testDestruction();
    testAsyncDecision(logs);
    testHeadlessApp();

    check("no log line ever contains an API key", !logs.anyContains(kKey) && !logs.anyContains(kEnvKey));

    std::printf("\n%s  (%d passed, %d failed)\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    std::fflush(stdout);
    return g_fail ? 1 : 0;
}
