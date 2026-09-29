# tests

Headless behavioral tests for tcxJev, run automatically in CI. **You don't need
to run this by hand**: it's a console program that asserts the addon's behavior
and exits non-zero on failure. It needs no API key and no network: every HTTP
call goes to a scripted fake server through `Client::setTransport()`.

What it covers: the request JSON for every question type and the raw form,
response parsing, errors and retries (400 / 401 / 422 / 429 / 529 / 5xx / network /
non-JSON bodies), request ids, async FIFO order, which thread listeners run
on (plain vs `tc::Deliver::Main`), sync mode, destroying the client with work
pending (including from inside its own listeners), and that the API key never
reaches a log line.

CI (`TrussC-org/ci-actions`) builds and runs it on macOS / Windows / Linux.
To run it locally anyway:

```bash
trusscli update
trusscli run
```
