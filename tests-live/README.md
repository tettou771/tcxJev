# Manual live API runner

The key is read **only from `TYPESAFE_API_KEY`**, never printed. With it unset
or empty, the runner prints `SKIP: TYPESAFE_API_KEY not set` and exits 0.
**Images are uploaded to TypeSafe's external API and may incur usage charges.**
Supply the key through your environment, never through command arguments.

Build against the same local TrussC checkout as `tests/`, then run (replace the
absolute image path):

```bash
env -u TYPESAFE_API_KEY GIT_OPTIONAL_LOCKS=0 /home/toru/TrussC-dev/trussc-ai-tools/tcbuild.sh cmake -S /home/toru/TrussC-dev/codex-work/w-tcxjev/tests-live -B /tmp/tcxjev-live -DTRUSSC_ROOT=/home/toru/TrussC-dev/TrussC -DCMAKE_BUILD_TYPE=Debug
env -u TYPESAFE_API_KEY GIT_OPTIONAL_LOCKS=0 /home/toru/TrussC-dev/trussc-ai-tools/tcbuild.sh cmake --build /tmp/tcxjev-live -j4
/tmp/tcxjev-live/tests-live /absolute/path/to/tower.jpg
```

`CMakeLists.txt` comes from `trusscli update --ide cmake`, with a standalone
`TRUSSC_ROOT` branch added to build this addon checkout into `/tmp`.
Regeneration removes that branch. `addons.make` lists tcxCurl and tcxJev.

The headless App runs four requests sequentially, with retries disabled.
Use an image whose short side remains >=280 px after each maxSide resize.
Auto uses JPG for opaque input, PNG for transparent input. Request 4 uses an
area-averaged CPU copy with long side <=200 px and no maxSide.

Output has one line per request. `bytes` measures the actual encoded image
part, excluding JSON and multipart framing. Unsent images have `bytes=0 format=-`.
Box x,y,w,h are normalized; missing tokens/answers are `-`.
Kinds are local validation, known API error types, HTTP status, network,
response parsing, or admission errors. Request 4 appends
`transport=not_invoked` or `transport=invoked` from the public transport hook.
Exit codes: 0 after completion (including API errors), 1 if request 4 fails its
local-rejection check, 2 for image/runner errors. Library logs and curl verbose
output are disabled; error text, headers and raw bodies are never printed.

Offline checks (no key lookup, no network) use generated pixels and the addon's
public `Client::setTransport()` hook:

```bash
env -u TYPESAFE_API_KEY /tmp/tcxjev-live/tests-live --self-test
env -u TYPESAFE_API_KEY /tmp/tcxjev-live/tests-live --self-test-errors
env -u TYPESAFE_API_KEY ctest --test-dir /tmp/tcxjev-live --output-on-failure
```

They decode the measured bytes independently, check JPG signatures and request
1/2 dimensions (1568x882, 640x360), require fewer bytes for request 2, and require
request 4 to fail locally without transport. The error variant returns synthetic
401 bodies containing a header-echo sentinel; CTest fails if headers or that
sentinel appear. Live API compatibility remains for the manual run.
