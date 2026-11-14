# Support expansion

This is the implementation checklist for the additional support requested after
the initial protocol matrix passed. Existing protocol behavior remains covered;
new capabilities require their own tests and installed-package verification.

- [x] Protobuf Editions: tested edition range, generator features, imported and
  nested messages, compiled client/server calls and unsupported-range errors.
- [x] TLS shutdown: drain RPCs, coordinate reader/writer completion, exchange
  close-notify within a deadline, retain explicit abort/cancellation semantics.
- [x] Native trust: macOS/Windows platform verification and Linux system roots,
  explicit private-CA overrides, hostname/trust-policy failures and CI coverage.
- [x] Managed transport: URL parsing, DNS/connect/TLS deadlines, HTTP/2 reuse,
  bounded HTTP/1 pools, idle expiry, new-call reconnection, no implicit retries,
  cancellation and joined shutdown on application-owned executors.
- [x] Protobuf Lite: MessageLite binary codecs and generated services, standalone
  lite-only link, bounded protocol JSON without full-runtime reflection,
  explicit application-JSON capability handling and package/CI configurations.
- [ ] Documentation/examples and final compiler, sanitizer, conformance and
  installed-package checks on Linux, macOS and Windows.

Work is in the primary checkout; no task worktree was created.
[PR #1](https://github.com/happenings-dk/connect-cc/pull/1) contains the expansion.

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

Hosted Linux GCC/Clang/ARM64, macOS, Windows and TSan pass the expansion
baseline, as do full/Lite packages and parser fuzzing. Final verification adds
fatal UBSan, OpenSSL's upstream dependency-specific sanitizer configuration,
deadline saturation and a post-DNS cancellation guard. The last checkbox remains
open until that hosted matrix finishes. System DNS cancellation remains
cooperative; see [MANAGED_TRANSPORT.md](MANAGED_TRANSPORT.md).
[HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md) records exact source revisions.
