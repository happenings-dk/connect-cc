# Support expansion

This is the implementation checklist for the additional support requested after
the initial protocol matrix passed. Existing protocol behavior remains covered;
new capabilities require their own tests and installed-package verification.

- [ ] Protobuf Editions: tested edition range, generator features, imported and
  nested messages, compiled client/server calls and unsupported-range errors.
- [ ] TLS shutdown: drain RPCs, coordinate reader/writer completion, exchange
  close-notify within a deadline, retain explicit abort/cancellation semantics.
- [ ] Native trust: macOS/Windows platform verification and Linux system roots,
  explicit private-CA overrides, hostname/trust-policy failures and CI coverage.
- [ ] Managed transport: URL parsing, DNS/connect/TLS deadlines, HTTP/2 reuse,
  bounded HTTP/1 pools, idle expiry, new-call reconnection, no implicit retries,
  cancellation and joined shutdown on application-owned executors.
- [ ] Protobuf Lite: MessageLite binary codecs and generated services, standalone
  lite-only link, bounded protocol JSON without full-runtime reflection,
  explicit application-JSON capability handling and package/CI configurations.
- [ ] Documentation/examples and final compiler, sanitizer, conformance and
  installed-package checks on Linux, macOS and Windows.

Work is in the existing primary checkout; no task worktree or PR has been created.

## Implementation checkpoint

Editions 2023–2024, full/Lite codec overloads and a Lite-only CMake target,
bounded protocol JSON, native macOS/Windows trust policy, coordinated TLS
shutdown, and managed transport are implemented. Local checks cover generated
Editions calls, Lite-only linkage and installed consumption, streaming and
pool admission over both HTTP versions/TLS, idle replacement, handshake
shutdown/deadlines, and cooperative/uncooperative TLS shutdown.

The expanded local full run passes 236 CTest entries including all ten
conformance profiles. Thirteen Lite tests and the installed Lite consumer pass;
the macOS native trust probe against `https://connectrpc.com` passes. ASan/UBSan
passes the same 236 entries, including caller cancellation and trust replacement. The README
managed example compiles and runs against the installed package.

Hosted Linux GCC/Clang/ARM64, macOS and TSan pass the expansion. Windows header
portability and Linux ASan dependency fixes are being verified; packages and
fuzzing are still running. The checkboxes remain open until final evidence
and documentation are complete. See [HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md).
