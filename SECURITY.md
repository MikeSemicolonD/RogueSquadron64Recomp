# Security Policy

## Supported versions

Only the latest release, and `main`, get fixes.

## Reporting a vulnerability

Report it privately, not in a public issue: use **Report a vulnerability** on the repository's [Security tab](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/security/advisories/new).

Please include:

- what is affected (online co-op networking, mod loading, save files, the browser tools, ...)
- steps or a file that reproduces it
- the release version or commit, and platform

Areas that take untrusted input and matter most:

- **Online co-op:** packets from the other player (ENet6 / Steam relay).
- **Mods:** `.nrm` code mods and native mods run with full access to your machine. **Only install mods you *trust*!** This is by design, not a vulnerability.
- **Browser tools** (`tools/web/`): files loaded into the explorer and save editor.

This is a hobby project, so there is no response-time guarantee, but reports are taken seriously.

Vulnerabilities in a dependency (RT64, N64ModernRuntime, N64Recomp, ENet6, SDL) should also be reported upstream.
