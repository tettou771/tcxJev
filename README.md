# tcxJev

A [TrussC](https://github.com/TrussC-org/TrussC) client for **Jev**, TypeSafe's
System One model.

Jev makes fast, structured decisions for software. You send it a **state** (a
piece of text, or JSON such as a chat log or a record) and a set of named,
typed **questions**; it returns one calibrated answer per question: a
**Noul** gives the probability that a yes/no question is yes, a **Choice**
picks one option from a set you define (with a probability for every option),
and a **Score** places the state on an ordered rubric of levels. The answers
are numbers your code can branch on, not generated text. See the
[TypeSafe docs](https://docs.typesafe.ai) (API reference:
[docs.typesafe.ai/api](https://docs.typesafe.ai/api)).

## Install

List both addons in your project's `addons.make` (tcxJev uses tcxCurl for
HTTPS):

```
tcxCurl
tcxJev
```

`trusscli addon add tcxJev` adds both lines for you (`addon.json` declares
the tcxCurl dependency). tcxCurl ships with TrussC; on Linux it needs
`sudo apt install libcurl4-openssl-dev`.

You also need a TypeSafe API key (see [API key](#api-key)).

[`example-basic/`](example-basic) sends one request with two Nouls, a Choice
and a Score about a support message and draws the answers as bars; it reads
the key from `bin/data/secrets.json`.

## Quick start

```cpp
#include <TrussC.h>
#include <tcxJev.h>
using namespace std;
using namespace tc;
using namespace tcx::jev;

class tcApp : public App {
    Client jev;                 // reads the TYPESAFE_API_KEY environment variable
    EventListener jevListener;

    void setup() override {
        // Deliver::Main: run this listener on the main thread (start of the
        // next frame), where it may touch nodes and draw state.
        jevListener = jev.responseEvent.listen([](ResponseEventArgs& e) {
            if (!e.ok) {
                logWarning() << "Jev request " << e.requestId << " failed: " << e.error;
                return;
            }
            logNotice() << "urgent:      " << e.answers["urgent"].noul;        // 0..1
            logNotice() << "department:  " << e.answers["department"].choice;  // "billing"
            logNotice() << "frustration: " << e.answers["frustration"].score;  // 0..2
        }, Deliver::Main);

        jev.request(Request()
            .state("Help! My payouts have been failing for 3 days.")
            .noul("urgent", "Does this convey urgency?")
            .choice("department", "Which team should handle this?", {
                {"billing",   "Payments, invoicing, refunds"},
                {"technical", "Bugs, outages, integrations"},
                {"sales",     "Pricing, upgrades, new accounts"},
            })
            .score("frustration", "How frustrated is the customer?",
                   {"Calm", "Frustrated", "Very angry"}));
    }
};
```

One request can carry many questions; they are all evaluated against the same
state in one call. Batching the questions about one state into one request is
about 10x cheaper and faster than one request per question (TypeSafe's
[parallel questions](https://docs.typesafe.ai/cookbooks/parallel_questions)
cookbook). Different states need separate requests; those run concurrently
(see [Many requests](#many-requests)).

## Building a request

| Method | Sends |
|--------|-------|
| `state(json)` | The content to evaluate: a string, or a JSON object / array. |
| `noul(name, instructions)` | A yes/no question. |
| `noul(name, instructions, whenTrue, whenFalse)` | Same, with descriptions of what yes / no mean (`criteria.true` / `criteria.false`; pass `nullptr` for one side to leave it out). |
| `choice(name, instructions, options)` | Pick one option. `options` is an object option -> description (`nullptr` for none) or an array of names: `Json::array({"red", "green", "blue"})`. At most 255 options. |
| `score(name, instructions, levels)` | Rate against ordered levels, lowest first (2 to 10; the API also accepts a single level, but tcxJev warns about it). A `vector<string>` works too. |
| `question(name, json)` | Any question object, sent as-is (escape hatch for new API fields). |
| `model(name)` | Model for this request only. |

`instructions`, option descriptions and levels can be strings or structured
JSON, as the API allows (see
[Advanced: structure](https://docs.typesafe.ai/primitives/advanced)):

```cpp
jev.request(Request()
    .state(Json{{"sender", "payroll@example.com"}, {"message", "Reply with your password"}})
    .noul("asks_credentials",
          Json{{"question", "Does the `message` ask for a credential?"}, {"inspect", "message"}},
          "Asks the recipient to send a password, PIN or one-time code",
          "No credential is requested"));
```

There is also a raw form that takes the API's `questions` object directly:
`jev.request(state, questionsJson)`.

## Reading the response

`responseEvent` fires once for every request that `request()` accepted:
answered, failed or cancelled. `ResponseEventArgs`:

| Field | Meaning |
|-------|---------|
| `requestId` | The id `request()` returned (ids start at 1 and increase). |
| `ok` | `true` when `answers` is valid. |
| `cancelled` | `true` when `cancel()` / `cancelAll()` dropped it before it was sent. |
| `statusCode` | HTTP status of the last attempt; `0` = no HTTP response (network error, timeout). |
| `error` | What went wrong (empty when `ok`). |
| `model` | The versioned model that answered, e.g. `jev-1.13.0`. |
| `answers` | `map<string, Answer>`, one per question name. |
| `inputTokens` / `outputTokens` | Usage. |
| `raw` | The full response body as JSON (the error body on failure; a non-JSON body is kept as a JSON string). |

`Answer`:

| Field | Question type | Meaning |
|-------|---------------|---------|
| `type` | all | `QuestionType::Noul` / `Choice` / `Score` (`Unknown` for anything else). |
| `noul` | Noul | Probability of yes, 0..1. |
| `choice` | Choice | The most probable option key. |
| `score` | Score | Probability-weighted position along the levels (`0` = first level); can land between levels. |
| `confidence` | Choice, Score | 0..1, how certain the answer is. Nouls have none (the `noul` value is the whole distribution). |
| `probabilities` | Choice, Score | Option -> p, or level index as a string (`"0"`, `"1"`, ...) -> p. |
| `legend` | Score | Level index -> description (a structured level is kept as its JSON text). |

Fields a response does not carry keep their defaults (0 / empty), and
unexpected types never throw.

## Threading and delivery

`setAsync()` decides where the HTTP call runs, and `responseEvent` fires on
that same thread:

| `setAsync` | HTTP call runs | `responseEvent` fires |
|------------|----------------|------------------------|
| `true` (default) | On worker threads, up to `setMaxConcurrent()` requests at a time (default 10), started in request order | On the worker that ran it, as each response arrives: in completion order (in request order with `setMaxConcurrent(1)`) |
| `false` | Inside `request()`, on the calling thread (blocks, retries included) | On the calling thread, before `request()` returns |

Like every `tc::Event` fired off the main thread (network receive events, for
example), each listener chooses where it runs:

| Listener | Runs |
|----------|------|
| `responseEvent.listen(fn, Deliver::Main)` | On the main thread: immediately if the event fires there, otherwise at the start of the next frame (before `update()`), in order. Use this for anything that touches nodes, GPU or draw state. |
| `responseEvent.listen(fn)` | Right where the event fires (the worker in async mode). No frame latency, but it must not touch the scene. |

- `request()` returns an id right away (async) so you can match responses to
  requests. It returns `0`, and logs why, when the request could not be queued
  (no questions, no API key, web build).
- Listeners are never called concurrently, even with several workers: one
  response fires at a time.
- Worker threads start as requests queue up, never more than
  `setMaxConcurrent()`, and stay (idle) until the client is destroyed; a
  client that sends one request at a time uses one.
- `Deliver::Main` needs the TrussC frame loop (`runApp` / `runHeadlessApp`);
  in a plain `main()` without it, listen without `Deliver::Main`.
- Switching `setAsync(false)` does not cancel requests already queued; they
  finish on the workers.
- Destroying the `Client` drops queued requests, and nothing fires after the
  destructor returns (`Deliver::Main` calls still waiting for a frame are
  dropped with the event). It waits for HTTP calls already in progress (they
  end together, so at most one timeout) and for a listener running on another
  thread. It is safe to destroy the client from inside its own listener.

## Many requests

Questions about one state belong in one request. When the states differ
(classifying a thousand texts, say), send one request each; the client runs up
to `setMaxConcurrent()` of them at once (default 10), so a few hundred short
requests take seconds instead of minutes. Just call `request()` in a loop and
match the answers by id:

```cpp
map<uint64_t, int> idToText;   // request id -> index into texts

void tcApp::classifyAll() {    // once, not every frame
    for (int i = 0; i < (int)texts.size(); i++) {
        uint64_t id = jev.request(Request()
            .state(texts[i])
            .choice("topic", "What is this text about?", topics));
        idToText[id] = i;
    }
}

// in the Deliver::Main listener:
//     results[idToText[e.requestId]] = e.answers["topic"].choice;
```

- **Responses arrive in completion order.** Match them by `requestId`, not by
  arrival. If you show "the latest answer" for an input that keeps changing
  (text being typed, say), an older request can finish after a newer one;
  ignore it by id, which only ever grows:

  ```cpp
  if (e.requestId > shownId) { shownId = e.requestId; show(e); }
  ```

  `setMaxConcurrent(1)` sends one request at a time and keeps request order.
- **Don't queue faster than it drains.** A `request()` in `update()` on every
  frame queues 60 per second. `getPendingCount()` (requests whose response has
  not fired yet) lets you throttle: `if (jev.getPendingCount() < 3) jev.request(...)`.
- **Drop what is no longer wanted.** `cancel(id)` removes a request that has
  not been sent yet and `cancelAll()` empties the queue; each dropped request
  still fires once, right away on the calling thread, with `cancelled` set.
  Requests already being sent can't be cancelled (they are billed anyway).
- The API allows 1200 requests per minute per key (see
  [Errors and retries](#errors-and-retries) for what happens beyond that).

## API key

- `Client()` reads the `TYPESAFE_API_KEY` environment variable (the same
  name the official SDKs use).
- `Client(key)` / `setApiKey(key)` set it explicitly (surrounding whitespace
  is trimmed, so a key read from a file works as-is).
- The key is sent only as the `Authorization: Bearer` header. tcxJev never
  logs it, never enables curl's verbose output, and replaces it with
  `<redacted>` if an error body happens to echo it.
- Don't commit keys. One way to keep it out of the code: put it in
  `bin/data/secrets.json` (`secrets.*` is in the standard project
  `.gitignore`) and load it in `setup()`:

  ```cpp
  if (fileExists(getDataPath("secrets.json"))) {   // {"typesafe_api_key": "..."}
      Json secrets = loadJson("secrets.json");
      if (secrets.contains("typesafe_api_key") && secrets["typesafe_api_key"].is_string()) {
          jev.setApiKey(secrets["typesafe_api_key"].get<string>());
      }
  }
  ```

## Errors and retries

| Status | Meaning | Retried |
|--------|---------|---------|
| 400 | The request was rejected: an invalid question or model (e.g. a Choice with no options, more than 10 Score levels, an unknown model); `error` carries the reason | no |
| 401 | Missing or invalid API key | no |
| 422 | The request body failed schema validation (e.g. a missing field such as `state`) | no |
| 429 | Rate limited | yes, pauses the whole client |
| 529 | Overloaded | yes, pauses the whole client |
| 408, other 5xx | Timeout / server error | yes |
| 0 | No HTTP response (connection failure, timeout) | yes |

- Retries: `setMaxRetries(n)` (default 3 retries after the first attempt, 0
  disables every retry), with exponential backoff `setRetryDelay(initial, max)`
  (default 0.5 s doubling up to 8 s, minus up to 25% random jitter). A
  `Retry-After` header is not honored (tcxCurl does not expose response
  headers).
- 429 and 529 mean "slow down", not "this request is broken", so they are
  handled differently: the whole client pauses (no request of this client is
  sent until the pause ends), then the request is sent again. The pause starts
  at the retry delay and doubles, up to 30 s, while the limit keeps answering
  429; the in-flight requests that hit the limit at the same moment share one
  pause instead of stretching it. These retries don't count toward
  `setMaxRetries()`: a request gets up to 10 of them (about two minutes with
  the defaults) before it fails with the 429 / 529. So a large batch slows
  down to what the limit allows instead of failing.
- `setTimeout(seconds)` bounds each HTTP attempt (default 30 s).
- Failures are also logged as warnings (`[tcxJev]`), retries and pauses as
  notices.

## Custom transport

`setTransport(fn)` replaces the HTTP call, e.g. to route through your own
proxy (so the app doesn't hold the key) or to fake the server in tests:

```cpp
jev.setTransport([](const TransportRequest& req) {
    TransportResponse res;
    // POST req.body (JSON) to req.url with "Authorization: Bearer " + req.apiKey,
    // timeout req.timeoutSeconds; fill statusCode + body (or error when there
    // is no response).
    return res;
});
```

It runs on a worker thread (async) or the calling thread (sync), up to
`getMaxConcurrent()` calls at a time per client, so it must be thread-safe
(or call `setMaxConcurrent(1)`). With a custom transport `request()` doesn't
require an API key. An empty function restores the default tcxCurl transport.

## Platforms

| Platform | Status |
|----------|--------|
| macOS / Windows / Linux | Supported, built and tested by CI |
| Android | Should work through tcxCurl's Android backend (async default); not tested yet |
| Web | Not supported: tcxCurl has no web backend and there are no worker threads. `request()` logs an error and returns 0; `setAsync(true)` logs an error and async stays off |

## Tests

`tests/` is a headless, network-free test (a fake transport stands in for the
server). CI builds and runs it on macOS / Windows / Linux. To run it locally:

```bash
cd tests
trusscli update
trusscli run
```

## License

MIT. See [LICENSES.md](LICENSES.md).
