# BUGBOT.md

Review policy for this repo. Bugbot must apply every rule below to every PR.
Block (request changes / leave a comment) on any violation; do not warn-only.

> Bugbot loads `.cursor/BUGBOT.md` files but does **not** auto-follow links to
> other markdown files. Each rule below therefore inlines a short summary so
> Bugbot has enough context. The linked file is the **source of truth** — when
> reviewing, open and read it before commenting.

This is a vendored fork of upstream WebRTC (m88-era). Almost every PR here is
an Ambient patch to the signalling-thread latency of a WebRTC message path, so
the review is about **which thread work runs on** and **when resources are
allocated and released** — not about upstream's style or design.

Every check below was caught in a real review on #3, #4, #6 or #7. Prefer
quoting the check by name and citing the `file:line` given.

---

## How to review a PR

1. **Name the thread for every hunk.** Use the `RTC_RUN_ON` /
   `RTC_DCHECK_RUN_ON` annotation, the `_w` / `_n` / `_s` suffix, or the target
   thread of the enclosing lambda. If the thread cannot be named from the diff,
   say so and ask for the annotation — the rest of the review depends on it.
2. **For every blocking call added or moved**, state the calling thread and the
   target thread, then walk the callee chain looking for a call back to the
   blocked thread. A blocking `Invoke` parks the caller in
   `rtc::Thread::Send` → `socketserver()->Wait(kForever, false)`, which does
   **not** drain the caller's message queue.
3. **For every `PostTask` added**, state the lifetime of every captured pointer
   and which guard makes the task safe to run late.
4. **For every statement relocated to another thread**, audit its *callee
   graph*, not its own body. Most of a relocated block usually already ran on
   the destination thread; the bug is in the one callee that did not.
5. Apply **Repository policies** to the diff as a whole, then run the
   **Mandatory checks**.

If a rule and the diff appear to conflict, prefer the rule. If the rule is
genuinely wrong, the PR must update the rule file in the same change.

---

## Rule sources

Open each file before reviewing matching changes. The summary is a hint, not a
substitute for reading the file.

| Rule file | Applies to (paths) | What it enforces (summary) |
|---|---|---|
| `.claude/rules/thread-affinity.md` | `api/**`, `pc/**`, `media/**`, `video/**`, `call/**`, `p2p/**`, `rtc_base/**` (`*.cc`, `*.h`) | Inside a blocking `worker_thread()->Invoke` / `network_thread()->Invoke`, every proxy-typed call MUST go through `internal()` — the proxy marshals back to the blocked thread. Only `rtc::Thread::Invoke` / `Send` are safe waits; a `condition_variable` / bare `Event::Wait` neither short-circuits on `IsCurrent()` nor pumps. Signalling, worker and network may be the same `rtc::Thread`. No `receiver_internal()` / `sender_internal()` on a Plan-B-reachable path. Safety must be structural, not held up by call-site ordering. Fix the primitive, not N call sites. |
| `.claude/rules/async-teardown.md` | `pc/**`, `video/**`, `media/**`, `call/**` (`*.cc`, `*.h`) | Disable before you release, and post in that order. Every async teardown needs a queue-side guard set at the HEAD of the posted task. Sync and async forms share one post and one flag. Fire-and-forget teardown needs exactly one drain point reached from every mutating entry. Idempotency flags are read only on the thread that writes them. Null the handle after releasing it. A destructor that offloads a release MUST `std::move` sole ownership into the lambda. `PostTask` capturing `this` needs a `WeakPtr` or a moved `scoped_refptr` — this tree has no `PendingTaskSafetyFlag`. |
| `.claude/rules/ambient-fork.md` | every session | Every behaviour change is gated on `AmbientFlags::MessageExecutionOptimization()` and the flag-off branch is **textually upstream**, duplication included. Both branches are written out in full rather than sharing a parameterised body. A new flag reachable from `api/` must not pull a non-`api/` header (`api/DEPS` / `gn check`). There is no CI on this repo; verification is a container build plus a load run. |

If a new `.claude/rules/*.md` file is added, the same PR MUST add a row here
and a row in the `CLAUDE.md` conventions table.

---

## Mandatory checks

Run all three groups on every PR, regardless of which files changed.

### Deadlock and thread affinity

- [ ] **Proxy call inside a blocking `Invoke`.** Every method declared under
      `BEGIN_SIGNALING_PROXY_MAP` marshals to the signalling thread
      (`pc/rtp_transceiver.h:226-244` declares thirteen). Inside a
      `worker_thread()->Invoke` / `network_thread()->Invoke` lambda, any such
      call must go through `internal()`. The Ambient same-thread fast path
      tests `IsCurrent()` on the **target** thread, so from the worker it takes
      the marshal path and still hangs (`api/proxy.h:293-307`); `PROXY_METHOD1`
      through `PROXY_METHOD5` have no fast path at all. This is a release-build
      hang, not a DCHECK. Bad: `pc/peer_connection.cc:4079`. Good: `:4083`.
      Bugbot reported this exact deadlock twice on #4, and Plan B is still the
      default `sdp_semantics`.
- [ ] **Non-pumping wait.** `std::condition_variable::wait`, a bare
      `rtc::Event::Wait`, `Thread::Join` and `std::future::get` are hard blocks:
      unlike `Thread::Send` they neither short-circuit when the target is
      current (`rtc_base/thread.cc:871`) nor pump the caller's queue
      (`:899`). Converting a blocking `Invoke` to `PostTask` plus such a wait
      deadlocks whenever the posted-to thread is the caller. `BaseChannel`
      already documents that worker and network may be the same thread, and
      factories alias signalling with worker. Live example: PR #7's
      `DrainPendingChannelDestroys`.
- [ ] **Safety held up by call-site ordering.** "This is safe because callee X
      early-returns, since Y happens to run first" is a contract nothing
      enforces. Require the structural fix. Worked example:
      `VideoRtpReceiver::Stop()` (`pc/video_rtp_receiver.cc:127-147`) makes two
      blocking hops back to signalling and is dead code today only because
      `stopped_` is initialised `true`.
- [ ] **Relocated statement audited by callee graph.** The PR must name the
      callees of anything moved to a new thread and say which thread each
      already ran on.
- [ ] **`Invoke` to `PostTask` capture audit.** A blocking `Invoke` may capture
      by reference (`pc/peer_connection.cc:3673` uses `[&]`); a `PostTask` must
      copy into a local first and capture that by value.
- [ ] **Stale thread annotation.** `RTC_RUN_ON(x)` left on a function the change
      moved to thread `y` must be corrected or removed, not ignored. Note
      `RTC_DCHECK_RUN_ON` compiles out in release, so a wrong annotation is
      silent in production.
- [ ] **`receiver_internal()` / `sender_internal()` on a Plan-B path.** Both
      `RTC_CHECK_EQ(1u, …)` (`pc/rtp_transceiver.cc:219-230`), which aborts in
      release; Plan B leaves a transceiver with none or several. Loops over
      transceivers use `receivers()` / `senders()`. A null-check placed *after*
      such an accessor is unreachable and is itself a smell.

### Resource lifetime

- [ ] **Disable before release, and post in that order.** The correct sequence
      is `DisableMedia_w()` for every channel, then `StopEncodersAsync()` for
      every channel, then the destroy loop
      (`pc/peer_connection.cc:1148-1164`). The disable is itself asynchronous
      (`SetSend(false)` posts its own encoder-queue work), so ordering the
      *posts* is what orders the *effects*. An inverted order leaves a
      zero-bitrate update or an encode queued behind a teardown that already
      released the encoder.
- [ ] **Queue-side guard at the head of the posted teardown.** Work already
      queued behind the teardown must no-op. `stopped_` is set as the first
      statement of the posted task (`video/video_stream_encoder.cc:297`) and
      read at `:997` and `:1587`. Setting such a flag at the *end* leaves the
      release itself uncovered. `ReleaseEncoder()` does not null `encoder_`
      (`:1740`), so the guard is the only protection.
- [ ] **One posted body, one flag, for the sync and async forms.** `Stop()` and
      `StopAsync()` both call `PostStopTask()` and both branch on
      `stop_posted_` (`video/video_stream_encoder.cc:291-319`). Two separate
      posts, or an async form that does not set the flag, breaks the later
      blocking wait. A duplicated teardown body across the pair is a
      flaggable smell on its own — the copies drift.
- [ ] **Exactly one drain point, reached from every mutating entry.** Flag a new
      deferred or pending container whose drain is missed by any exit path,
      including error returns and `Rollback()`. PR #7 drains at
      `DestroyAllChannels`, `ApplyLocalDescription` and
      `ApplyRemoteDescription` but not `Rollback()`, which lets a queued
      `DeinitNetwork_n()` disconnect from a `JsepTransport` that
      `RollbackTransports()` already destroyed.
- [ ] **Cross-thread idempotency flag.** A `bool` guarding "already done" must
      be read only on the thread that writes it, or be `std::atomic` or
      lock-guarded. Good: `stopped_` is `RTC_GUARDED_BY(&encoder_queue_)`
      (`video/video_stream_encoder.h:202`). Bad: PR #7's `network_deinit_done_`
      in `pc/channel.{h,cc}` is written on the network thread by
      `DeinitNetwork_n()` and read on the worker by `Deinit()` as a plain
      `bool`.
- [ ] **Null the handle after releasing it.** PR #7's `DeinitNetwork_n()` calls
      `DisconnectFromRtpTransport()` without `rtp_transport_ = nullptr`, which
      leaves the guard and the pointer able to disagree.
- [ ] **Destructor offload transfers sole ownership.** Null the member, then
      **move** into the lambda: `[source = std::move(source)]`
      (`pc/video_track.cc:53-68`). A copy capture leaves the destructor's local
      able to drop the last reference on the very thread the change exists to
      get off — an intermittent race, not a compile error. Require an inline
      fallback if the thread cannot start.
- [ ] **`PostTask` capturing `this` or a raw pointer.** Needs an
      `rtc::WeakPtr` from a `WeakPtrFactory`, or a `scoped_refptr` moved into
      the lambda, or a documented process-lifetime object. This tree has **no**
      `PendingTaskSafetyFlag` / `SafeTask`. In-tree idioms:
      `video/video_send_stream_impl.cc:488-495`,
      `pc/data_channel_controller.cc:360-366`.
- [ ] **Async teardown moves observable completion past the API return.** If a
      `SetLocalDescription` / `SetRemoteDescription` observer can now fire while
      the encoder, playout or demuxer registration is still live, the PR must
      say so and say why it is acceptable.
- [ ] **Deferral opens an interval where an invariant is false.** The PR must
      name the invariant, the interval, who must not read it, and how a
      violation would present. A reader that `continue`s rather than returning
      an error turns the bug into a stream that silently never sends — see the
      comment at `pc/peer_connection.cc:3561-3566` for the required shape.
- [ ] **Teardown reordering versus release-build invariants.** Name the
      invariant relied on. `Call::~Call()` `RTC_CHECK`s that the stream maps are
      empty (`call/call.cc:479-486`) — a release abort, and the reason channels
      must die before the transports and the `Call`. `~VideoStreamEncoder`
      requires `Stop()` first (`video/video_stream_encoder.cc:284-287`).
- [ ] **Error-path parity with the code being replaced.** A batch must fail the
      way the per-element loop failed. `FlushPendingChannelCreates` stops at the
      first failed create, attaches the prefix and returns that error
      (`pc/peer_connection.cc:3670-3719`); it must not destroy the successes.
      New rollback behaviour is a behaviour change, not a batching change.
- [ ] **Removing proactive work names its reader.** The legacy stats sweeps were
      removable because the only reader, `GetStats(StatsObserver*, …)`, calls
      `UpdateStats` itself. State who reads the thing you are no longer
      producing, and what is lost.
- [ ] **Erasing from a container other code walks.** The predicate must make
      reuse impossible, and the cheap conditions must be tested first so the
      expensive walk only runs for candidates
      (`pc/peer_connection.cc:6235-6266`).

### Fork discipline

- [ ] **Gated, and flag-off is textually upstream.** Every behaviour change sits
      behind `AmbientFlags::MessageExecutionOptimization()`, and the flag-off
      branch matches upstream byte for byte — including whether upstream used
      the proxy or `internal()`, and including duplicated tails. Sharing code
      between the two branches to avoid duplication loses the guarantee; this
      codebase duplicates deliberately.
- [ ] **Fix the primitive, not N call sites.** `internal()` at a call site is
      correct only while that site provably stays on that thread, and nothing
      rechecks it when code moves. Use `internal()` where it is a *correctness*
      requirement (inside a worker `Invoke`), never as an optimisation — PR #3
      replaced ten hand-converted call sites with four macro edits covering
      every zero-argument proxy declaration in the tree.
- [ ] **A new flag reachable from `api/` pulls no non-`api/` header.**
      `api/DEPS` and `gn check` reject it; that is why `api/ambient_flags.h` is
      a header-only struct with a function-local static.
- [ ] **PR description carries its evidence.** It must state what is slow with
      an evidence tag (`MEASURED` with numbers, `INFERRED` from the code path,
      or a bare observed count), what changed and which guard makes each change
      safe, what was measured against an explicit "Not measured", and the one
      direction to check in review. A PR that changes thread affinity without
      saying which thread the work moved from and to is incomplete.

---

## Repository policies

Cross-cutting rules not owned by any single rule file. Apply to the diff as a
whole.

### Upstream fidelity

- Do not reformat, rename or tidy upstream code the change does not otherwise
  touch. A diff that is larger than the behaviour change is harder to rebase
  onto the next upstream roll.
- New Ambient code follows the surrounding upstream style, not a house style.
- Prefer adding a defaulted virtual (`virtual void StopEncodersAsync() {}`) over
  editing every implementation, so subclasses that do not participate are
  untouched.

### Doc updates that are required, not optional

- **New `.claude/rules/*.md`** → add a row to the `Rule sources` table above
  and to the `CLAUDE.md` conventions table in the same PR.
- **New `.claude/skills/<name>/`** → add a row to
  `.claude/skills/README.md` in the same PR.
- **New Ambient-added symbol** (a new method, flag or member that upstream does
  not have) → add it to the inventory in `.claude/rules/ambient-fork.md`.

### Filename and schema conventions

- `.claude/rules/`: lowercase, kebab-case, one topic per file. Every file starts
  with YAML frontmatter declaring `paths:`, unless the rule genuinely applies
  every session — which is rare and needs explicit justification in the PR
  description.
- `.claude/skills/<name>/SKILL.md`: skill name is lowercase, kebab-case.
  Frontmatter declares `name:` and `description:`.
- No instruction added to `CLAUDE.md` may contradict a rule in
  `.claude/rules/`. Flag conflicts.

---

## Out of scope

Bugbot does not need to flag:

- Upstream style — `.clang-format` owns this. Line wrapping, include order and
  brace placement are not review material.
- Upstream code the diff does not touch, including pre-existing bugs, unless the
  change makes one reachable.
- Missing unit tests. **There is no CI on this repo.** Verification is a build
  in the appliance container (`ninja owt` → `libowt.a`) plus a subscribe/hangup
  load run, and the PR body states coverage per change. Ask for honest coverage
  reporting, not for tests.
- `[CONN-DIAG]` diagnostics logged at `LS_ERROR`. The appliance sets OWT log
  severity to `kError`, which drops `LS_INFO` and `LS_WARNING`, so `LS_ERROR` is
  the only level that arrives. It is not a severity claim.
- The single shared `MessageExecutionOptimization` flag covering several
  changes. This was raised and accepted: the changes are turned on together
  atomically.
