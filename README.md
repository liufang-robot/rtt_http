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
invokes an operation or stages one input sample in a transport channel. The
component acquires that sample at its next cyclic input boundary. Every output
exposes a latest-value route backed by its committed snapshot; edits to an output
working image remain invisible until the component completes a successful cycle.
Multiple clients can read independently. RTT 3.0 or newer is required. Request and response limits are enforced without
truncating values.

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
