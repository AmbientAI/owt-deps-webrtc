# `.claude/rules/`

Path-scoped instructions for Claude. Each `.md` file here covers one topic.
Files with `paths:` frontmatter only enter Claude's context when Claude reads
files matching the glob; files without `paths:` load every session (use
sparingly).

This is a vendored fork of upstream WebRTC, so the globs name the directories
Ambient patches touch — not the whole tree.

## When to add a rule here

- The instruction only matters for one part of the tree.
- The instruction is short (under ~30 lines) for a new, narrow reminder.
  Multi-step procedures belong in `.claude/skills/<name>/SKILL.md` instead. The
  ~30-line budget is for *new* rules — an existing rule that is the single
  source of truth for a wide-reaching topic (thread affinity, teardown) may run
  much longer; don't split those up just to hit the budget.
- A code review caught something Claude should have known. Every rule in this
  directory is a review comment from #3, #4, #6 or #7 turned into a rule;
  follow that standard rather than adding generic C++ advice.

## When NOT to add a rule here

- The rule applies to every session → root `CLAUDE.md`, or `ambient-fork.md`
  if it is fork discipline.
- The rule is a multi-step procedure → make it a skill.
- The rule restates upstream WebRTC's own design → link to the code instead.
- The rule is really a review check → it still belongs here, but the same PR
  must add it to `.cursor/BUGBOT.md`. Bugbot does **not** read this directory.

## File schema

```markdown
---
paths:
  - "pc/**/*.cc"
  - "pc/**/*.h"
---

# <Topic>

- Specific, verifiable rules. One per bullet.
- Cite `file:line` on `origin/main` rather than describing code in prose.
- Quote the real before/after where a shape is easy to get wrong.
```

Omit `paths:` only for rules that genuinely apply to every session. If two
rules conflict, Claude may pick one arbitrarily — review periodically.

## Existing rules

| Filename | `paths:` | Purpose |
|---|---|---|
| `thread-affinity.md` | `api/**`, `pc/**`, `media/**`, `video/**`, `call/**`, `p2p/**`, `rtc_base/**` (`*.cc`, `*.h`) | Which thread work runs on, and how not to deadlock moving it: the proxy marshal rules and `internal()`, alias-safe waits, callee-graph audits, `Invoke` vs `PostTask` captures, thread annotations, `senders()` / `receivers()`, structural over incidental safety |
| `async-teardown.md` | `pc/**`, `video/**`, `media/**`, `call/**` (`*.cc`, `*.h`) | Resource allocation and release ordering: disable before release, queue-side guards, one shared post, single drain point, idempotency flags, destructor offload ownership, `PostTask` capture safety, release-build teardown invariants, error-path parity |
| `ambient-fork.md` | every session | Fork orientation, `AmbientFlags` gating and upstream fidelity, the gate-site list, the Ambient-added symbol inventory, and the fact that there is no CI |

If you add a file here, the same PR MUST add a row to this table, to the
`Rule sources` table in `.cursor/BUGBOT.md`, and to the conventions table in
`CLAUDE.md`.

## Suggested files (create as needed)

| Filename | Suggested `paths:` | Purpose |
|---|---|---|
| `sdp-negotiation.md` | `pc/**/*.cc`, `pc/**/*.h` | Unified Plan vs Plan B branch obligations, m= section recycling, rollback paths — referenced in passing by both topic rules but not yet written |
| `diagnostics.md` | `pc/**`, `video/**` | `[CONN-DIAG]` span conventions, why they log at `LS_ERROR`, when a span comes out — currently a paragraph inside `async-teardown.md` |

Neither is worth writing until a review surfaces a short, path-scoped reminder
that does not fit the two topic rules.
