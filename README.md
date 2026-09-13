# RTT HTTP service

This package implements an independent HTTP/REST interface for Orocos RTT
components. OCL owns its component service plugin and deployment lifetime.
The HTTP package owns JSON codecs, explicit static publication, the listener,
and bounded operation execution. It does not link to the OPC UA transport.

OCL provides `loadService("Deployer", "http")`, local listener configuration,
explicit `publishComponent(name)`, and coordinated deployment shutdown. Global
HTTP services, Deployer publication, live interface replacement, authentication,
WebSocket/SSE, and generic asynchronous jobs are outside this first version.

Build against a compatible RTT prefix, Boost.JSON 1.84 or later, and the
maintained cpp-httplib header. HTTPS uses OpenSSL by default and can be disabled
at build time with `RTT_HTTP_TLS=OFF`.

The native CI workflow builds the matching RTT 3 feature branch before HTTP.
The released development SDK supplies third-party dependencies; cyclic RTT
headers, libraries, and plugins are selected from the isolated CI install.

```sh
cmake -S . -B build -DRTT_HTTP_HTTPLIB_INCLUDE_DIR=/path/to/patched/cpp-httplib
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build
```

The maintained dependency is `liufang-robot/cpp-httplib` (upstream v0.54.1 plus
owned sockets, SIGPIPE policy, and exact request routing). The native workflow
records the tested immutable revision. Configuration requires all four capability
markers; the build enables ownership, signal opt-out, and raw routing automatically. The header
is private to the HTTP implementation. Public SDK consumers link the shared
`rtt_http::rtt_http` target or use `rtt_http-${OROCOS_TARGET}` pkg-config metadata.

The native facade is `RTT::http::Server`. `start(options)` opens a fresh listener;
`publishComponent(component)` records its current interface; `stop()` joins
network workers while preserving publication and unfinished calls.
`beginShutdown()` closes admission and `finishShutdown()` drains application
calls before releasing component references. The embedding deployment must
keep published components, interface objects, and execution engines alive
until that final cleanup completes.

REST collections start at `/api/v1/components`. Component and nested service
descriptions list properties, attributes, operations, and ports with canonical
RTT type schemas. GET reads a value, PUT replaces a writable value, and POST
invokes an operation or stages one explicitly enabled input sample. Publication
adds no port connection: already-connected and running components remain
observable. Both directions expose `/ports/P/latest`. Input reads show the last
component-acquired image, including configured defaults; output reads show the
last committed value and report `hasSample: false` before the first commit.
An output working-image edit and an input producer's queued sample remain
invisible until their respective component boundaries. Multiple clients read
independent frozen snapshots. RTT 3.0 or newer is required. Request and response
limits are enforced without truncating values.
Whole-port readers retain their images when a port is removed and destroyed.
The removed port's metadata and member routes return 404; a replacement port
with the same name does not retarget the original publication's reader.

Input writing is disabled by default. After publication, configure a stopped
component with `server.enableInputWrite(component, "input", &error)` or an exact
member such as `"io.input.axes[2].position"`. The same dot/index selectors are
used by RTT expressions and connections. Output endpoints, invalid selectors,
and regions that overlap an existing writer are rejected. Calling
`disableInputWrite` with the same endpoint releases only that source while
affected components are stopped. Repeated disable succeeds for a valid input;
repeated enable reconnects a source disconnected through ordinary port management.
Final server shutdown releases its sources through RTT's
safe port teardown, which stops affected running owners; ordinary local
connections are preserved.

For an enabled whole input, POST `{"value": 18}` to `/ports/input/samples`.
For an enabled `input.y`, POST the selected scalar to
`/ports/input/members/y/samples`. Fixed indices use one percent-encoded selector
segment, for example `/ports/input/members/axes%5B2%5D.position/samples`.
Nested services retain their `/services/io/ports/input` prefix. These routes
accept complete values of the selected RTT type, without numeric type conversion
or partial-object merging. Successful writes return 204 and stage the latest
sample for the next input acquisition; acknowledgement does not mean the
component has consumed it.

Selected member metadata and `/latest` reads are available without enabling
writes. `/ports/input/members/y` describes that exact member's `rttType`, schema,
`latestHref`, and writable state. Port discovery includes `memberHrefTemplate`
and the enabled `inputWrites` regions. `samplesHref` is null unless that exact
region is enabled, and its samples route returns 404 when disabled. Invalid
JSON syntax returns 400; rejected sample types or shapes return 422; a
disconnected configured source returns 503. All latest-value routes remain
read-only.

Operation timeouts return 504 without cancellation or replay. Admitted calls
keep capacity and storage until completion, including while HTTP is stopped.
ClientThread operations use the bounded HTTP call executor; OwnThread operations
retain RTT engine dispatch. Changing executor width while calls remain is
rejected. An application operation that never returns can prevent clean final
shutdown. The application must ensure safe concurrent property/attribute access.

Configuration defaults to unauthenticated HTTP on `127.0.0.1:8080`. Explicit
network binding and optional HTTPS are supported; TLS does not add client
authorization. cpp-httplib measures keep-alive timeouts in whole seconds, so
`keepAliveTimeoutMs` must be a positive multiple of 1000. Names use UTF-8 with
one percent-encoded URL segment per RTT name; slash, percent and plus remain
distinct. Empty and dot-only names are rejected at publication because browsers
cannot reliably address them as named resources. Reverse proxies must preserve
the original encoded path; arbitrary proxy normalization is unsupported.

The Linux proxy contract test exercises Nginx with this location inside the
frontend's server block. `$request_uri` preserves the original encoded URI;
do not replace it with normalized `$uri` or add name-rewriting rules.

```nginx
location /api/ {
    proxy_pass http://127.0.0.1:8080$request_uri;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
}
```

Configure with `RTT_HTTP_TEST_NGINX=ON` to run this proof locally. The fixture
starts its own loopback proxy and tests distinct slash, percent, Unicode, plus,
and encoded-hash names for both reads and writes.

Custom typekits and components continue to depend only on RTT. A separate HTTP
transport registers `TypeProtocol` codecs in project-local RTT slot 1043. JSON
uses exact decimal strings for 64-bit integers, underlying integers for enums,
and explicit strings for non-finite floats. Composite assignment validates the
whole value before touching component storage. Codecs are trusted extensions
and must honor their conversion budgets and synchronize mutable internal state.

For existing RTT structure/sequence metadata, `makeReflectedTypeProtocol<T>`
adds JSON reflection and typed retained port sampling. Import the ordinary
typekit first and prepare these codecs outside RTT's `registerTransport()`
callback: that callback holds the type repository's nonrecursive lock, and
reflection/type lookups would reacquire it. The callback can use its supplied
`TypeInfo` pointer and prepared registrations. Automatic reflection before
first start supplies value support where metadata is complete; typed port
sampling requires the companion transport. Registration freezes before the
first bind attempt and remains frozen after bind failure and restart.
Do not register codecs in a plugin metadata getter. A plugin must remain loaded
once it has installed callbacks, even if another codec cannot be registered;
report unsupported mappings without throwing out of its load entry point.

Distribution integration and native platform validation remain delivery gates;
this package is not yet an installed distribution feature.
