# Security policy

ShiftDrop opens files people drag onto it, including files from the internet and from mail. A bug in
how it reads them could matter, so security reports are welcome.

## Reporting a problem

Please report it privately, not in a public issue:

1. Go to <https://github.com/CoPrime69/ShiftDrop/security/advisories/new> (the repository's
   **Security** tab, then **Report a vulnerability**).
2. Say what you found, which version or commit, and how to reproduce it. A sample file helps a lot.
   If the file is sensitive, say so and leave it out; we can agree on a way to share it.

The maintainer aims to answer within a week. Once a fix is out, the advisory is published with
credit to you, unless you would rather stay anonymous.

## Supported versions

Fixes go into the next release. Only the latest release gets them; there are no backports.

## What counts

Some examples of what is in scope:

- A crafted file that makes ShiftDrop or `ShiftDropWorker.exe` run code, crash in a way that looks
  exploitable, or write outside the folder it should write to.
- An archive that gets past the entry checks (`..`, absolute paths, device names, links) or the size
  limits.
- Anything that lets the sandboxed worker read the user's files or reach the network.
- Anything that makes ShiftDrop, or the bundled ffmpeg run it starts, connect to the network.
- An output that replaces an existing file, or an undo that deletes a file the user changed.

Out of scope: problems that need an attacker who already runs code as the same Windows user, and
bugs in Microsoft Office, LibreOffice or Windows itself (report those to their makers).

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#the-sandbox) explains what the sandbox does and does not
cover.
