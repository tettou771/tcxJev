# tests

Headless behavioral tests for tcxJev, run automatically in CI. **You don't need
to run this by hand**: it's a console program that asserts the addon's behavior
and exits non-zero on failure. It needs no API key and no network: every HTTP
call goes to a scripted fake server through `Client::setTransport()`.

What it covers: the request JSON for every question type and the raw form,
response parsing, errors and retries (400 / 401 / 422 / 429 / 529 / 5xx / network /
non-JSON bodies), the client-wide pause on 429 / 529, request ids, FIFO order
with `setMaxConcurrent(1)`, concurrent requests (the cap, completion order,
changing the cap at runtime, listeners never overlapping), the pending count
and cancellation, which thread listeners run on (plain vs `tc::Deliver::Main`),
sync mode, destroying the client with work pending (including from inside its
own listeners and with several calls in flight), and that the API key never
reaches a log line.

CI (`TrussC-org/ci-actions`) builds and runs it on macOS / Windows / Linux.
To run it locally anyway:

```bash
trusscli update
trusscli run
```

For a local checkout without running the project generator, point directly to
TrussC. Objects, libraries and the test binary go into the chosen build tree.
Core must have its current bundled shader headers (as the supplied checkout
does); if they need regeneration, use a writable copy of core instead:

```bash
env -u TYPESAFE_API_KEY GIT_OPTIONAL_LOCKS=0 \
  /home/toru/TrussC-dev/trussc-ai-tools/tcbuild.sh \
  cmake -S tests -B /tmp/tcxjev-native \
  -DTRUSSC_ROOT=/home/toru/TrussC-dev/TrussC -DCMAKE_BUILD_TYPE=Debug
env -u TYPESAFE_API_KEY GIT_OPTIONAL_LOCKS=0 \
  /home/toru/TrussC-dev/trussc-ai-tools/tcbuild.sh \
  cmake --build /tmp/tcxjev-native -j4
env -u TYPESAFE_API_KEY ctest --test-dir /tmp/tcxjev-native --output-on-failure
```

Image tests generate U8 pixels in memory and capture only injected transports.
They independently parse multipart framing, decode PNG/JPEG, exercise Auto
(including RGBA with one nonopaque pixel and gray-alpha), use core flip=1 in a
separate translation unit, and check budget admission/release, reference/local
validation, Box parsing and retry byte identity. Internal `packVision` accepts
synthetic encoded bytes for otherwise expensive file/body-limit tests and a
boundary generator for a deterministic collision test; production always uses
the real encoder, documented limits and generated boundaries.

A custom `std::numpunct<char>` regression installs grouping and a comma decimal
point without requiring a system locale, then restores the previous global
locale with RAII. It checks 45-byte ASCII asset references, inline scanning,
safe multipart boundaries, injected transport completion, JSON decimal bytes
and numeric response parsing. Score legend keys use complete ASCII integers
within `int` range; grouped, whitespace-prefixed, plus-prefixed, overflowing,
partial and empty keys are ignored.

Downscale tests decode 1920×1080 -> 1568×882, odd/portrait sizes and half-up
rounding, check flat colors and 2×2 checker averages, and reject disabled,
undersized or invalid resize cases without transport. Both snapshot and pending
budgets count the resized raw-channel payload, with exact-fit/one-byte-excess
and shared-copy checks. Auto RGBA selection includes a single nonopaque source
pixel whose averaged alpha rounds back to opaque.

The Box fixture in `src/vision.cpp` comes from the public Box guide, not a live
API capture. Nullable/malformed Box and 400/413/415/422 samples are explicitly
constructed. No auditor captures were supplied with this implementation;
compatibility with real 1.14 responses remains unverified until sanitized
captures with provenance are added. Tests never load secrets or contact the API.
