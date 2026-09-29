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
state in one call, which is much cheaper and faster than one request each.

## Building a request

| Method | Sends |
|--------|-------|
| `state(json)` | The content to evaluate: a string, or a JSON object / array. |
| `noul(name, instructions)` | A yes/no question. |
| `noul(name, instructions, whenTrue, whenFalse)` | Same, with descriptions of what yes / no mean (`criteria.true` / `criteria.false`; pass `nullptr` for one side to leave it out). |
| `choice(name, instructions, options)` | Pick one option. `options` is an object option -> description (`nullptr` for none) or an array of names: `Json::array({"red", "green", "blue"})`. At most 255 options. |
| `score(name, instructions, levels)` | Rate against ordered levels, lowest first (2 to 10). A `vector<string>` works too. |
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

`responseEvent` fires once for every request that `request()` accepted,
successful or not. `ResponseEventArgs`:

| Field | Meaning |
|-------|---------|
| `requestId` | The id `request()` returned (ids start at 1 and increase). |
| `ok` | `true` when `answers` is valid. |
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
| `true` (default) | On one worker thread, FIFO, one request at a time | On the worker, as each response arrives, in request order |
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
- The worker starts on the first async request.
- `Deliver::Main` needs the TrussC frame loop (`runApp` / `runHeadlessApp`);
  in a plain `main()` without it, listen without `Deliver::Main`.
- Switching `setAsync(false)` does not cancel requests already queued; they
  finish on the worker.
- Destroying the `Client` drops queued requests, and nothing fires after the
  destructor returns (`Deliver::Main` calls still waiting for a frame are
  dropped with the event). It waits for an HTTP call already in progress (at
  most the timeout) and for a listener running on another thread. It is safe
  to destroy the client from inside its own listener.

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
  Json secrets = loadJson("secrets.json");   // {"typesafe_api_key": "..."}
  if (secrets.contains("typesafe_api_key")) jev.setApiKey(secrets["typesafe_api_key"].get<string>());
  ```

## Errors and retries

| Status | Meaning | Retried |
|--------|---------|---------|
| 401 | Missing or invalid API key | no |
| 422 | The request failed validation; `error` carries the details | no |
| 429 | Rate limited | yes |
| 529 | Overloaded | yes |
| 408, other 5xx | Timeout / server error | yes |
| 0 | No HTTP response (connection failure, timeout) | yes |

- Retries: `setMaxRetries(n)` (default 3 retries after the first attempt, 0
  disables), with exponential backoff `setRetryDelay(initial, max)` (default
  0.5 s doubling up to 8 s, minus up to 25% random jitter). A `Retry-After`
  header is not honored (tcxCurl does not expose response headers).
- `setTimeout(seconds)` bounds each HTTP attempt (default 30 s).
- Failures are also logged as warnings (`[tcxJev]`), retries as notices.

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

It runs on the worker thread (async) or the calling thread (sync), one call at
a time per client. With a custom transport `request()` doesn't require an API
key. An empty function restores the default tcxCurl transport.

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
