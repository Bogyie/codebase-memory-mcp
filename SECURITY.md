# Security Policy

## Transparency & Disclaimer

codebase-memory-mcp interacts deeply with your filesystem. It reads source files across your entire codebase, writes to agent configuration files, and spawns background processes. This is inherent to what it does — not a bug.

**If you are uncomfortable with these access patterns**, please audit the source code before running. The full source is available in this repository. Fork releases are built in GitHub Actions and publish SHA-256 checksums. The fork's lightweight release workflow does not currently publish SLSA attestations, Sigstore bundles, or VirusTotal results; do not apply upstream provenance claims to a fork artifact unless that artifact actually includes them (see [Verification](#verification) below).

We are humans and can make mistakes. We take security seriously — it is Priority #1 for this project — but we cannot guarantee perfection. By using this software you accept responsibility for evaluating whether it meets your own security requirements.

## Runtime Network Behavior

Indexing, graph queries, semantic search, and MCP tool handling run locally. The
MCP server does not upload source code, repository paths, graph indexes, query
contents, environment variables, usage metrics, or telemetry.

The MCP server has one best-effort external runtime check: after MCP
`initialize`, it starts a background update-check thread that requests release
metadata from
`https://api.github.com/repos/bogyie/codebase-memory-mcp/releases/latest`.
That request is used only to show an update notice when a newer release exists.
It sends no project data; only standard HTTPS metadata, such as the destination
host and the normal `curl` request headers, are visible to GitHub and the
network path.

The update check is non-blocking for MCP startup and tool calls. If the machine
is offline, DNS fails, GitHub is unreachable, or `curl` exits with an error, the
check is ignored. The request is also bounded with `curl --max-time 5`; a
process shutting down immediately while the check is still running may wait for
that bounded background thread to finish.

Set `CBM_DISABLE_UPDATE_CHECK=1` (or `true`) before starting the MCP server to
disable this request. Explicit install and update commands remain available.

Explicit install, package-manager, and `codebase-memory-mcp update` flows are
separate user-initiated network operations that download release assets and
checksums from GitHub.

## Help Us Stay Secure

**We actively invite security researchers to try to break this project.**

If you find a vulnerability — anything from a logic bug to a remote code execution — we want to know. You will receive a fast response, public credit (if you want it), and the knowledge that you helped make a tool used by developers worldwide more secure.

What we consider in scope:

- Arbitrary code execution via MCP tool inputs or CLI arguments
- File reads or writes outside the indexed project root
- Shell injection through any code path
- Binary tampering or supply chain attacks
- Privilege escalation or sandbox escapes

Please report **privately** rather than as a public issue so we can fix before public disclosure. See below for how.

## Reporting a Vulnerability

If you discover a security vulnerability, please report it **privately** so we
can fix it before public disclosure:

1. **Do NOT open a public issue, PR, or social-media post** for security
   vulnerabilities.
2. **Email:** send the report to [boggyhint@gmail.com](mailto:boggyhint@gmail.com)
   with subject `[security] codebase-memory-mcp`. GitHub private vulnerability
   reporting is not currently enabled for this fork, so the advisory URL is not
   a working intake channel.
3. Include: description, reproduction steps, affected version, and potential
   impact.
4. Include your **GitHub handle and a contact email**. We use these to credit
   you and to invite you (read-only) to privately verify the fix before its
   release — see step 4 of the
   [handling process](docs/SECURITY-DISCLOSURE.md#what-happens-after-you-report).
   Let us know if you would prefer to remain anonymous.

> **This is a solo, volunteer-maintained project, so security handling is
> best-effort.** As good-faith targets — not guarantees — we aim to:
>
> - **acknowledge** your report within **7 days** (usually much sooner);
> - give an **initial assessment and severity** within **14 days**;
> - **develop, validate, and release a fix** as quickly as the severity
>   warrants — typically within **90 days**, and expedited for high-severity
>   issues.
>
> If something will take longer, we will tell you and keep you updated.

We follow **coordinated disclosure**: fixes are developed privately, validated
across all supported platforms, released, and only then disclosed publicly via a
[GitHub Security Advisory](https://github.com/bogyie/codebase-memory-mcp/security/advisories)
with a **CVE** and credit to you. The full handling process — including how you
can verify the fix before release — is documented in
[`docs/SECURITY-DISCLOSURE.md`](docs/SECURITY-DISCLOSURE.md).

### Safe harbor

We will not pursue or support legal action against researchers who act in good
faith — accessing only their own test data, avoiding privacy violations and
service disruption, and giving us reasonable time to fix before public
disclosure. Research conducted under this policy is considered authorised.

## Security Measures

This project implements multiple layers of security verification. The fork's
CI and release workflows are distinct, so the guarantees below are scoped to
the workflow that actually produced an artifact.

### Build-Time CI

- The repository includes an **8-layer security audit suite**:
  - Layer 1: Static allow-list for dangerous calls (`system`/`popen`/`fork`) + hardcoded URLs
  - Layer 2: Binary string audit (URLs, credentials, dangerous commands)
  - Layer 3: Network egress monitoring via strace (Linux)
  - Layer 4: Install output path + content validation
  - Layer 5: Smoke test hardening (clean shutdown, residual processes, version integrity)
  - Layer 6: Graph UI audit (external domains, CORS, server binding, eval/iframe)
  - Layer 7: MCP robustness (23 adversarial JSON-RPC payloads)
  - Layer 8: Vendored dependency integrity (SHA-256 checksums, dangerous call scan)
- **All dangerous function calls** require a reviewed entry in `scripts/security-allowlist.txt`
- **Time-bomb pattern detection** — scans for `time()`/`sleep()` near dangerous calls (could indicate delayed activation)
- **MCP tool handler file read audit** — tracks file read count in `mcp.c` against an expected maximum (detects added file reads that could exfiltrate data through tool responses)
- **CodeQL SAST** — static application security testing on configured pushes
  (taint analysis, CWE detection, data flow tracking). Fork releases wait for
  the current commit's CodeQL run and fail closed on open first-party alerts.
  Vendored grammar findings are handled separately by the pinned-dependency
  integrity audit rather than being silently counted as fork-owned source.
- **Fuzz testing** — configured jobs feed random/mutated inputs to the MCP server and Cypher parser. This catches crashes and memory errors that structured tests can miss.
- **Native antivirus jobs** are defined for supported platforms:
  - **Windows**: Windows Defender with ML heuristics — the same engine end users run
  - **Linux**: ClamAV with daily signature updates
  - **macOS**: ClamAV with daily signature updates

### Release-Time

The current `fork-release.yml` workflow used for new releases from
`bogyie/codebase-memory-mcp` requires lint, the repository security gate, the
cross-platform test matrix, and smoke tests of the exact platform archives
before publishing them with an exact `checksums.txt`. It intentionally does
**not** claim SLSA provenance, Sigstore signing, an SBOM attestation,
VirusTotal gating, or package-registry publication.

That stronger gate is not retroactive. Fork releases through `v0.9.0` used the
earlier build-and-checksum workflow. A release inherits only the controls shown
by the workflow run that produced its assets.

The inherited full `release.yml` workflow supports attestations, Sigstore,
SBOM, VirusTotal, and draft-then-publish verification. Those controls apply
only to a release that was actually produced by that workflow and includes the
corresponding evidence. Repository history or shared source alone is not proof
that a particular fork artifact passed those controls.

### Code-Level Defenses

- **Shell injection prevention** — production Git, curl, process-listing, and update paths use bounded argument-vector subprocesses with trusted executable resolution rather than a command shell
- **SQLite authorizer** — blocks `ATTACH`/`DETACH` at engine level (prevents file creation via SQL injection)
- **Loopback UI boundary** — the graph UI binds to loopback, validates Host and Origin before routing, and requires same-origin JSON for state-changing requests
- **Rooted source reads** — repository files are opened relative to a validated root and rejected when symlinks, reparse points, type changes, or generation changes break the read contract
- **Process-kill restriction** — cancellation is routed through the server-owned child control rather than accepting an arbitrary external PID
- **Atomic verified updates** — the update command bounds downloads, verifies the exact archive and checksum bytes, installs atomically, and accepts the new binary only after its `--version` probe succeeds

### Verification

For a fork release, download the selected archive and `checksums.txt` from the
same GitHub release. Select exactly that filename's entry before verification;
running `sha256sum -c checksums.txt` with only one archive present would also
report every other platform asset as missing.

```bash
ARCHIVE=codebase-memory-mcp-linux-amd64-portable.tar.gz
awk -v file="$ARCHIVE" 'NF == 2 && $2 == file { print; found=1 } END { if (!found) exit 1 }' \
  checksums.txt > "$ARCHIVE.sha256"
sha256sum -c "$ARCHIVE.sha256"
```

On macOS, use `shasum -a 256 -c "$ARCHIVE.sha256"` for the final command. If a future fork release
includes a GitHub attestation or Sigstore bundle, verify its subject, repository,
and signer workflow against `bogyie/codebase-memory-mcp`; do not substitute the
upstream repository identity.

## Supported Versions

| Version | Supported |
|---------|-----------|
| Latest release | Yes — security fixes land in the newest release |
| Older releases | No — please upgrade to the latest release |

Only the latest release is supported. Security fixes are shipped in a new
patched release rather than backported to older versions; upgrading to the
newest version is the supported path to receive them.
