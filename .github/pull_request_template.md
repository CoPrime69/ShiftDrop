## What this changes

<!-- One or two sentences. Link the issue if there is one: Fixes #123 -->

## Why

<!-- The problem it solves, or what was wrong before. -->

## How it was tested

- [ ] Unit tests added or updated (or: nothing here can be tested without a person at the mouse)
- [ ] CI is green
- [ ] Hand-tested sections of docs/TESTING.md: <!-- for example "K, L" -->

## Checklist

- [ ] No network access, telemetry or run-time downloads added
- [ ] No GPL or AGPL code; any new dependency is in vcpkg.json, scripts/make-notices.ps1 and THIRD-PARTY-NOTICES.md
- [ ] Outputs go through StagedOutput, so nothing is overwritten and every result can be undone
- [ ] README and docs updated if users or contributors will notice the change
