#pragma once

// =============================================================================
// tcxJev - client for TypeSafe's Jev model (System One API) for TrussC
// =============================================================================
// Jev answers typed questions about a "state" (text or JSON) with calibrated
// numbers instead of generated text:
//   - Noul   : a yes/no question -> probability of yes (0..1)
//   - Choice : pick one option from a set -> chosen key + per-option probability
//   - Score  : place the state on an ordered rubric -> position between levels
// Docs: https://docs.typesafe.ai
//
//   tcx::jev::Client jev;   // API key from the TYPESAFE_API_KEY env var
//   tc::EventListener listener = jev.responseEvent.listen(
//       [](tcx::jev::ResponseEventArgs& e) {
//           if (e.ok) tc::logNotice() << "urgent: " << e.answers["urgent"].noul;
//       });
//   jev.request(tcx::jev::Request()
//       .state("Help! My payouts have been failing for 3 days.")
//       .noul("urgent", "Does this convey urgency?"));
//
// Threading (defaults): requests run one at a time on a worker thread, and
// responseEvent fires on the main thread at the start of the next frame.
// setAsync() / setResponseEventAsync() change that; see README.md.
// =============================================================================

#include <TrussC.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace tcx::jev {

enum class QuestionType { Noul, Choice, Score, Unknown };

// One answer per question, stored under the question's name.
// Fields a response does not carry keep their defaults.
struct Answer {
    QuestionType type = QuestionType::Unknown;
    double noul = 0;                             // Noul: probability of yes (0..1)
    std::string choice;                          // Choice: the selected option key
    double score = 0;                            // Score: position along the levels (0 = first level)
    double confidence = 0;                       // Choice / Score (Noul has none: 0)
    std::map<std::string, double> probabilities; // Choice: option -> p, Score: "0".."n" -> p
    std::map<int, std::string> legend;           // Score: level index -> description
                                                 // (a structured level is kept as its JSON text)
};

// Payload of Client::responseEvent. Fired once per request that request()
// accepted (non-zero id), success or failure.
struct ResponseEventArgs {
    uint64_t requestId = 0;    // the id request() returned
    bool ok = false;           // true: answers are valid
    int statusCode = 0;        // HTTP status of the last attempt, 0 = no HTTP response
    std::string error;         // why it failed (empty when ok)
    std::string model;         // versioned model that answered, e.g. "jev-1.13.0"
    std::map<std::string, Answer> answers;
    int inputTokens = 0;
    int outputTokens = 0;
    tc::Json raw;              // full response body (or error body); a body that
                               // is not JSON is kept as a JSON string
};

// Builder for one request: the state plus any number of named questions.
// Every question name comes back as a key of ResponseEventArgs::answers.
//
//   Request()
//       .state(tc::Json{{"message", "My card was charged twice."}})
//       .noul("refund", "Is the customer asking for a refund?")
//       .choice("department", "Which team should handle this?", {
//           {"billing", "Payments, invoicing, refunds"},
//           {"technical", "Bugs, outages, integrations"},
//       })
//       .score("frustration", "How frustrated is the customer?",
//              {"Calm", "Frustrated", "Very angry"});
//
// Instructions, option descriptions and levels can each be a string or
// structured JSON (object / array), as the API allows.
class Request {
public:
    // The content to evaluate: a string, or a JSON object / array.
    Request& state(const tc::Json& s);

    // Yes/no question. The answer's `noul` is the probability of yes.
    Request& noul(const std::string& name, const tc::Json& instructions);
    // Same, with descriptions of what yes and no mean (pass nullptr to leave
    // one side undescribed).
    Request& noul(const std::string& name, const tc::Json& instructions,
                  const tc::Json& whenTrue, const tc::Json& whenFalse);

    // Pick one option. `options` is either an object option -> description
    // (description may be nullptr) or an array of option names:
    //   {{"billing", "Payments"}, {"technical", nullptr}}
    //   tc::Json::array({"red", "green", "blue"})
    // The API accepts at most 255 options.
    Request& choice(const std::string& name, const tc::Json& instructions,
                    const tc::Json& options);

    // Rate against ordered levels, lowest first (2..10 levels):
    //   {"Calm", "Frustrated", "Very angry"}
    // A std::vector<std::string> converts too.
    Request& score(const std::string& name, const tc::Json& instructions,
                   const tc::Json& levels);

    // Escape hatch: a question object sent as-is, e.g.
    //   {{"type", "noul"}, {"instructions", "..."}}
    Request& question(const std::string& name, const tc::Json& rawQuestion);

    // Model for this request only (default: the client's model).
    Request& model(const std::string& m);

    const tc::Json& getState() const { return state_; }
    const tc::Json& getQuestions() const { return questions_; }
    const std::string& getModel() const { return model_; }

    // The request body as sent to POST /v1/systemone.
    tc::Json toJson(const std::string& defaultModel = "jev-latest") const;

private:
    tc::Json state_;                          // null until state() is called
    tc::Json questions_ = tc::Json::object();
    std::string model_;                       // empty = the client's model
};

// What a Transport receives: one HTTPS POST.
struct TransportRequest {
    std::string url;             // https://api.typesafe.ai/v1/systemone
    std::string apiKey;          // send as "Authorization: Bearer <apiKey>"; never log it
    std::string body;            // JSON, send with "Content-Type: application/json"
    float timeoutSeconds = 30;
};

// What a Transport returns.
struct TransportResponse {
    int statusCode = 0;          // HTTP status; 0 = no response (network error, timeout)
    std::string body;
    std::string error;           // what went wrong when statusCode is 0
};

using Transport = std::function<TransportResponse(const TransportRequest&)>;

namespace detail {
    // The async setting that takes effect: async needs worker threads.
    constexpr bool resolveAsync(bool requested, bool threadsAvailable) {
        return requested && threadsAvailable;
    }
} // namespace detail

class Client {
public:
    // API key from the TYPESAFE_API_KEY environment variable, if set.
    Client();
    explicit Client(std::string apiKey);
    // Stops the worker: pending requests are dropped and nothing fires after
    // this returns. Waits for an HTTP call already in progress (bounded by
    // setTimeout()).
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client& setApiKey(std::string apiKey);   // never logged
    bool hasApiKey() const;

    Client& setModel(std::string model);     // default "jev-latest"; "" resets it
    std::string getModel() const;

    Client& setTimeout(float seconds);       // per HTTP attempt, default 30
    float getTimeout() const;

    // Retries after the first attempt, default 3. Retried: 408, 429, 5xx
    // (including 529 Overloaded) and network errors, with exponential backoff.
    Client& setMaxRetries(int retries);
    int getMaxRetries() const;
    // Backoff before retry n (0-based): min(initial * 2^n, max), minus up to
    // 25% random jitter. Default 0.5 s / 8 s.
    Client& setRetryDelay(float initialSeconds, float maxSeconds = 8.0f);

    // Queue a request. Returns its id (> 0, increasing), which the matching
    // ResponseEventArgs carries. Returns 0 when it could not be queued (no
    // questions, no API key, web build); the reason is logged as an error.
    uint64_t request(const Request& req);
    // Raw form: `questions` is the API's name -> question object map.
    uint64_t request(const tc::Json& state, const tc::Json& questions);

    // Fired once per accepted request. Thread and timing: see setAsync() and
    // setResponseEventAsync().
    tc::Event<ResponseEventArgs> responseEvent;

    // true (default where threads exist): requests run on one worker thread,
    // FIFO, one at a time. false: request() runs the HTTP call (and its
    // retries) on the calling thread and fires responseEvent before returning.
    // Where threads are unavailable (web) setAsync(true) logs an error and
    // async stays false.
    Client& setAsync(bool async);
    bool isAsync() const;
    static bool isAsyncSupported();

    // false (default): responses fire on the main thread, in request order, at
    // the start of the next frame (before update). Needs the TrussC frame loop.
    // true: responses fire right away on the thread that produced them (the
    // worker). Switching to true first fires every queued response, in order.
    Client& setResponseEventAsync(bool async);
    bool isResponseEventAsync() const;

    // Replace the HTTP call, for tests or custom networking (a proxy, another
    // HTTP stack). It runs on the worker thread (async) or the calling thread
    // (sync), one call at a time per client. An empty function restores the
    // default tcxCurl transport. With a custom transport, request() does not
    // require an API key.
    Client& setTransport(Transport transport);

private:
    struct State;                 // defined in tcxJev.cpp
    std::shared_ptr<State> state_;
};

} // namespace tcx::jev
