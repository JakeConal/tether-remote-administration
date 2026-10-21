# Readiness decisions and allowed-use policy record

This file records the public-repo readiness decisions that tether's design
(`openspec/changes/tether-v2-reset/design.md`, "Open Questions") left to the
project owner, together with the owner's sign-off, and the results of the
allowed-use policy verification. Decided in the task 7.3 sign-off round,
2026-10-21 (owner sign-off given interactively this session).

## Owner decisions (task 7.3)

| Decision                       | Choice                            | Consequence                                                                                                                        |
|--------------------------------|-----------------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| Final license                  | **Keep MIT as-is**                | `LICENSE` remains exactly as carried since the founding commit (`Copyright (c) 2024 Nam Anh`). Nothing to regenerate or re-deliver.  |
| README tone                    | **Portfolio-professional**        | The README is written for readers outside the course: purpose and scope first, authorized-use up front, quickstart, command table, architecture overview. The course framing stays in `report/` and the OpenSpec documents. |
| `report/` (LaTeX course report)| **Remove at publish**             | The report is NOT linked from the README or docs. When the repository is made public, `report/` is removed from the tree in its own commit; it remains retrievable from history (the `v1.0-final` tag and the owner's report commits). |

Sign-off: owner, via the interactive decision round in the apply session of
2026-10-21 (options offered per decision; answers recorded verbatim above).

Publish-time checklist for the owner (the only future action this record
requires): before going public, delete `report/` in a single commit — nothing
else in this record needs re-verification at publish time.

## Allowed-use policy results (task 7.4)

Policy: tether is an **authorized-use lab tool**. It may only drive machines
the operator owns or is explicitly authorized to administer; the keylogger and
keyboard-lock surfaces exist as explicit, audit-visible console commands (no
stealth features by design); destructive commands are refuse-by-default at the
console until confirmed with `--yes` (or an interactive y/N answer).

Search method: `git grep` across the full tracked tree for (a) the three
retired v1 admin addresses, (b) `admin.txt` references, (c) credential-shaped
strings (`client_secret`, `refresh_token`, app-password patterns, SMTP/IMAP
URLs, `@gmail.com`) in code and config files, (d) v1's hardcoded port
(`12345`) and long high-entropy literals. Executed 2026-10-21.

| Search                                | Result in the tracked tree                                                                                                              |
|---------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|
| v1 admin addresses (`hieunguyen.jc@…`, `tiendat1243oo@…`, `phamnamanh25@…`) | **None.** They survive only in git history (the `v1.0-final` founding snapshot), which is the intentional rollback anchor. |
| `admin.txt`                           | Only in prose inside `openspec/**` documents describing its retirement. No file, code path, or config field carries the name.            |
| Credential-shaped strings             | Only OAuth **field names** (`client_secret`, `refresh_token`) in the Gmail adapter's config/protocol handling and in tests using fabricated values. Tether reads credentials and tokens from files at operator-configured paths **outside the repository** (`GmailSectionConfig::credentials_path`, `AgentConfig::token`; `lib/transport/gmail.hpp` documents this). |
| Port `12345` / hardcoded addresses    | Only the historical comment in `lib/common/config.hpp` noting that v2 replaced them. All endpoints come from config files (agent default dial port 47474 ships as a config-file default, not source wiring). |
| High-entropy literals                 | The RFC 4231 HMAC test vector, the base64 alphabets, and test-fabricated tokens — no secrets.                                            |

Conclusion: **no credentials or admin email lists appear anywhere in source**;
`admin.txt`-style files and Gmail credentials live only at operator config
paths outside the repo. The policy holds as of this record; new code must keep
it (any tree change that reintroduces a literal address or secret is a policy
violation, not a refactor).
