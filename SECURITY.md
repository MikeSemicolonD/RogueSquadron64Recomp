# Security Policy

## Supported versions

Only the latest release, and `main`, get fixes.

## Verifying a release download

Every release lists each file's SHA-256 in `SHA256SUMS.txt` and next to the file under Assets. Download `SHA256SUMS.txt` into the same folder as your files, then check them:

- Windows (PowerShell): `Get-FileHash <file> -Algorithm SHA256`, then compare the hash with the file's line in `SHA256SUMS.txt` (case does not matter)
- Linux: `sha256sum -c SHA256SUMS.txt --ignore-missing`
- macOS: `shasum -a 256 -c SHA256SUMS.txt --ignore-missing`

On Linux and macOS each file you have should print `OK`. If a hash does not match, delete the file and download it again from the [Releases page](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/releases). If it still does not match, report it as below.

Only download releases from that page. A hash copied from anywhere else proves nothing.

A matching hash only shows that the download is complete. To confirm that this repository's release workflow built the file, use the [GitHub CLI](https://cli.github.com/):

```sh
gh attestation verify RogueSquadron64Recomp-<platform>-<arch>-<version>.<ext> --repo MikeSemicolonD/RogueSquadron64Recomp
```

`<platform>-<arch>` is `windows-x64`, `windows-x86`, `linux-x64`, `macos-universal` or `android-arm64`.

This works for the game, relay server and mod tool releases. A file that fails this check was not built by the release workflow, so don't run it.

## Network access

- **The game** goes online only when you use the multiplayer option. The first time you host or join, your firewall may ask to let it through. Allow it to play co-op, or deny it and play offline.
- **The relay server** (`rs64_relay`) accepts connections from other players, so your firewall will ask for network access the first time it runs. It needs this to work.

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
