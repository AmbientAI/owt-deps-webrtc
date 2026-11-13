---
name: optimize-message-path
description: Audit a WebRTC message type or function in this fork for cross-thread work that can be batched, posted, or offloaded, and propose a gated change. Use when asked to speed up, profile, or reduce latency on a WebRTC message path (an SDP apply, Close, a channel or encoder teardown, an ICE or RTP path), when asked why a signalling-thread stall happens, or when asked what could be optimised in a given function. Produces a hop table, a recipe match from RECIPES.md, the guard that keeps it safe, and a PR write-up in the house format. Do not use for upstream-behaviour bugs unrelated to threading or resource lifetime.
---

# Optimize Message Path

Given a **message type** (an SDP apply, `Close`, a publish or hangup, a rejected
m= section) or a **function**, find the cross-thread work on its path and
propose a change following the pattern of PRs #3, #4, #6 and #7.

Two categories, in priority order:

1. **Reduce cross-thread work that can deadlock** — collapse per-element
   blocking hops, or remove the hop entirely.
2. **Make resource allocation and release asynchronous** — take a queue wait, a
   thread join or a capturer destroy off the latency-sensitive thread.

Read `.claude/rules/thread-affinity.md` and `.claude/rules/async-teardown.md`
before proposing anything. This skill is the procedure; those are the
correctness rules, and a proposal that violates one of them is not a proposal.

## When to use

- "Why does `Close` take seconds?" / "Speed up the SDP apply path."
- "What can we optimise in `PeerConnection::<fn>`?"
- Investigating a multi-second signalling-thread stall.
- Reviewing whether an existing hop is still needed after an upstream roll.

## When NOT to use

- A correctness bug unrelated to threading or resource lifetime → fix it
  directly; there is no optimisation to propose.
- Reviewing someone else's diff → use `.cursor/BUGBOT.md`'s checks, not this.
- The subject is outside `api/`, `pc/`, `media/`, `video/`, `call/`, `p2p/` →
  the thread map here will not apply.

## Steps

```
Task Progress:
- [ ] 1. Fix the subject and its entry thread
- [ ] 2. Build the thread-crossing call graph
- [ ] 3. Tabulate and cost the hops
- [ ] 4. Match a recipe
- [ ] 5. Satisfy the invariant, write the guard
- [ ] 6. Gate it
- [ ] 7. Verify
- [ ] 8. Write it up
```

### 1. Fix the subject and its entry thread

Resolve the request to one entry point and name the thread it enters on. A
message type maps to a function: an SDP apply is `ApplyLocalDescription` /
`ApplyRemoteDescription`, a hangup is `Close` → `DestroyAllChannels`, a rejected
m= section is `UpdateTransceiverChannel`.

```bash
git grep -n "RTC_RUN_ON\|RTC_DCHECK_RUN_ON" origin/main -- pc/peer_connection.h | rg '<Fn>'
```

If no annotation exists, fall back to the `_w` / `_n` / `_s` suffix or the
target thread of the enclosing lambda. **If you cannot name the entry thread,
stop and say so** — everything downstream depends on it.

### 2. Build the thread-crossing call graph

All searches run against `origin/main`; this checkout may be on a branch
without the tree.

```bash
# Blocking hops on the path.
git grep -n "Invoke<\|->Send(" origin/main -- pc/ video/ media/ call/

# Posted work.
git grep -n "PostTask\|PostDelayedTask" origin/main -- pc/ video/ media/ call/

# Hard waits: these neither short-circuit nor pump. Each one is a hazard.
git grep -n "condition_variable\|\.Wait(\|->Join()\|future<" origin/main -- pc/ video/ media/ call/

# Which methods marshal, and to where.
git grep -n "PROXY_METHOD\|PROXY_CONSTMETHOD\|PROXY_WORKER\|BEGIN_.*PROXY_MAP\|_THREAD_DESTRUCTOR" origin/main -- api/ pc/

# Thread affinity of a callee.
git grep -n "RTC_RUN_ON\|RTC_DCHECK_RUN_ON\|RTC_GUARDED_BY" origin/main -- <file>
```

Walk transitively. **Audit a candidate for relocation by its callee graph, not
its own body** — most of a block usually already ran on the destination thread,
and the bug is in the one callee that did not.

### 3. Tabulate and cost the hops

Produce this table. It is the deliverable of the audit even if no change is
proposed.

| Call site | From | To | Blocking? | Per-element? | What it actually does there |
|---|---|---|---|---|---|

Then classify each row against the four things worth removing:

- **Per-element hop in a loop** — cost is N round trips; the highest-value,
  lowest-risk find.
- **A wait on something with its own queue** — an encoder stop, a send-stream
  stop. Fan-out applies when the units own separate queues.
- **A wait on something with no queue** — a capturer destroy, a thread join.
  These need a dedicated thread, not a fan-out.
- **Proactive work with no reader** — deletable, once you name the reader.

Tag every cost claim. `MEASURED` needs a number and a source. `INFERRED` means
from the code path, not measured — say so rather than implying otherwise. A bare
observed count (`4026 entries against 12 m-lines`) is fine.

### 4. Match a recipe

Read `RECIPES.md` and pick one. Eleven performance recipes, three discipline
recipes, each with the before/after shape, the invariant, the guard, and a real
`file:line`.

Prefer, in order: remove the work entirely > collapse N hops into one > keep the
wait but move what it waits on > make it async. Async is last because it is the
only one that changes observable completion ordering.

If nothing matches, say so. A new recipe needs the same four parts and a row
added to `RECIPES.md` in the same change.

### 5. Satisfy the invariant, write the guard

Every recipe names an invariant. State how your change preserves it, then write
the guard. Re-check the proposal against:

- `.claude/rules/thread-affinity.md` — does anything in a new blocking lambda
  call back to the blocked thread? Is every new wait alias-safe? Did any
  relocated statement's callees get audited? Are `PostTask` captures copies?
- `.claude/rules/async-teardown.md` — is the disable posted before the release?
  Is there a queue-side guard at the head of the posted task? Is there exactly
  one drain point, reached from every mutating entry including `Rollback()`? Are
  idempotency flags read only on the thread that writes them? Does the error
  path still behave like the code being replaced?

If the safety argument is "callee X early-returns, since Y happens to run
first", the proposal is not finished. Restructure so the unsafe call is
unreachable.

### 6. Gate it

Behind `AmbientFlags::MessageExecutionOptimization()`, with the flag-off branch
textually upstream — duplication included. See `.claude/rules/ambient-fork.md`
for the three gate shapes and why fidelity beats deduplication.

### 7. Verify

There is no CI on this repo.

```bash
ninja owt        # in the appliance container; must produce the full libowt.a
```

Then a subscribe/hangup load run against the resulting `webrtc_server`. Record
publishes, unpublishes, fatals, and whether the path you changed was actually
reached. The local client typically never answers the offer, so renegotiation
paths are usually **not** exercised — report that rather than glossing it.

### 8. Write it up

Use the template below. Section names drift a little across #3/#4/#6/#7 but the
sequence does not: framing, then cost, then changes, then evidence, then the
limits of that evidence, then risk.

## Output template

````markdown
<One paragraph: which investigation this came from, what it is stacked on,
which flag gates it. State explicitly that with the flag off each site runs the
upstream code unchanged.>

## What is slow

<Mechanism first, then the cost with an evidence tag. One paragraph per
distinct cost.>

Example shape:
> Closing a PeerConnection destroys its video channels one at a time, and each
> destroy waits for that channel's encoder queue before the next one starts, so
> the Close costs the sum of the stops. Separately, `~VideoTrack` releases the
> video source on the signalling thread, which destroys the capturer and joins
> its frame generator thread, MEASURED at 414-2437 ms per track with other
> peers' signalling queued behind it.

## Changes

**1. `<Symbol>`** — <what it does>. <The guard, and why the guard is
sufficient.> <What is deliberately absent, e.g. "There is no completion
callback; the destroy loop's `Stop()` is the wait.">

**2.** …

<Where a change is subtle, add:>

Two things worth review attention here:

- <the non-obvious consequence, and why it is safe>
- <the ordering change, if any, and why it cannot bite>

## Measured

Built and run in <where>. <What the build produced.>

- Compiles and links clean.
- Under a <N>-worker subscribe/hangup load: **<N> publishes, <N> unpublishes,
  zero errors, zero fatals**.
- <Which release-build invariant did not fire, e.g. `~Call()`'s `RTC_CHECK` on
  empty stream maps.>

**How much of this is actually covered, stated precisely:** <the limitation of
the harness, with numbers>.

| Change | Path | Exercised locally |
|---|---|---|
| 1 — <name> | `Close` | Yes, full teardown |
| 2 — <name> | apply | Initial offer only |
| 3 — <name> | apply, needs a rejected section | No |

<One sentence saying which half is real coverage and which is compilation and
review only, and what would close the gap.>

**Not measured:** <the saving, or whichever part was not timed>. They are
reported as what they are — <e.g. strictly fewer blocking round trips on the
signalling thread, of unquantified benefit>.

## Risk

<Rank the changes against each other. Name the single riskiest one and why.>

<Then the one direction to check, e.g.:>
> The deadlock direction is the thing to check in review: any proxy call made
> from inside one of these worker lambdas would marshal back to the blocked
> signalling thread. Every such call in the new code goes through `internal()`.
````

Rules for filling it in:

- **Never claim a saving you did not measure.** "Not measured" is always
  present, and states what the change *is* rather than what it might be worth.
- **The coverage table is per change, not per PR.** "No" is an acceptable
  answer; hiding it is not.
- **Risk ranks, then points.** Reviewers need to know which hunk to read first
  and what to look for in it.
- Credit review provenance inline, in the body and in commit trailers:
  `Reported by Bugbot on #4.`, `Suggested by aditya-ambient in review.`

## Additional resources

- For the recipe catalogue, see [RECIPES.md](RECIPES.md)
- For the correctness rules any proposal must satisfy:
  `.claude/rules/thread-affinity.md`, `.claude/rules/async-teardown.md`
- For the flag gate and the Ambient-added symbol inventory:
  `.claude/rules/ambient-fork.md`
- For the review checks the resulting PR will be held to: `.cursor/BUGBOT.md`
