# Optimization Recipes

The shapes that landed in PRs #3, #4 and #6, plus the one currently blocked in
#7. Each recipe gives the before shape, the after shape, the **invariant** that
must be preserved, the **guard** used, and a real `file:line` on `origin/main`.

Used by `.claude/skills/optimize-message-path/SKILL.md` step 4. Correctness
rules for all of them are in `.claude/rules/thread-affinity.md` and
`.claude/rules/async-teardown.md`.

Prefer, in order: **remove the work** (P10) > **collapse N hops into one**
(P1-P3, P6) > **move what the wait waits on** (P7, P11) > **make it async**
(P4, P5). Async is last because it is the only family that changes observable
completion ordering.

## Index

| # | Recipe | Category |
|---|---|---|
| P1 | Per-element blocking `Invoke` in a loop → one batched `Invoke` | collapse |
| P2 | Split the batch: keep the marshalling-sensitive half where it was | collapse |
| P3 | Fold an adjacent independent hop in via a defaulted parameter | collapse |
| P4 | Blocking `Invoke` → `PostTask` plus an idempotent later wait | async |
| P5 | Sequential teardown → fan out the posts, then wait once | async |
| P6 | Split one blocking hop into its per-thread halves | collapse |
| P7 | Heavy destructor work → a dedicated thread | offload |
| P8 | Remove per-call proxy machinery with a same-thread fast path | remove |
| P9 | Remove proactive work that has no reader | remove |
| P10 | Bound an unbounded container | remove |
| P11 | Skip work the batch already did, without changing the rest | collapse |
| D1 | Prefer the primitive-level fix over N hand-converted call sites | discipline |
| D2 | Refactor so deferred work has exactly one drain point | discipline |
| D3 | Write the gate so the flag-off path is textually upstream | discipline |

---

## P1 — Per-element blocking `Invoke` in a loop → one batched `Invoke`

The highest-value, lowest-risk find. Cost goes from N round trips to one.

**Before** — N iterations on the signalling thread, each calling a helper that
takes its own `worker_thread()->Invoke`
(`pc/peer_connection.cc:1118-1127`, preserved as the flag-off path):

```cpp
for (const auto& transceiver : transceivers_) {
  if (transceiver->media_type() == cricket::MEDIA_TYPE_VIDEO) {
    DestroyTransceiverChannel(transceiver);   // its own Invoke, per element
  }
}
```

**After** — collect on the calling thread, one `Invoke`, loop inside
(`pc/peer_connection.cc:1136-1164`).

**Invariant:** same work, same order, same thread for each unit. Only the
round-trip count changes.

**Guards:** every proxy read inside the lambda goes through `internal()`; no
`_internal()` accessor with an `RTC_CHECK` precondition; error semantics match
the per-element loop (P-D2 and the error-path rule); nothing in the lambda's
transitive callee graph hops back to the blocked thread.

**Instances:** `DestroyAllChannels`, `PushdownMediaDescription`, channel
creates, receiver sink detach, rejected m-line destroys — five in PR #4 alone.

---

## P2 — Split the batch: keep the marshalling-sensitive half where it was

When one statement in the loop body cannot move, split the loop rather than
abandoning the batch.

**Before** — the whole body inside the worker `Invoke`, including
`SetChannel(nullptr)`, whose callee `VideoRtpReceiver::Stop()` makes two
blocking hops back to signalling.

**After** — `pc/peer_connection.cc:1131-1148`: detach loop on signalling,
only the destroy inside the `Invoke`:

```cpp
// Detach on the signaling thread: SetChannel(nullptr) stops the receivers,
// whose callees marshal to the signaling thread. Destroy video channels first
// since they may have a pointer to a voice channel.
std::vector<cricket::ChannelInterface*> video_channels;
std::vector<cricket::ChannelInterface*> audio_channels;
for (const auto& transceiver : transceivers_) {
  cricket::ChannelInterface* channel = transceiver->internal()->channel();
  if (!channel) { continue; }
  transceiver->internal()->SetChannel(nullptr);
  ...
}
worker_thread()->Invoke<void>(RTC_FROM_HERE, [&] { /* destroy only */ });
```

**Invariant:** the split point is exactly where the callee graph stops reaching
back to the calling thread.

**Guard:** trace the callees of each *statement*, not the statement itself, and
leave a comment recording the reason so a later refactor does not undo it.

---

## P3 — Fold an adjacent independent hop in via a defaulted parameter

Two consecutive hops become one without restructuring either caller.

**Before** — `EnableSending()` (one `Invoke`) immediately followed by
`PushdownMediaDescription()` (another), on every apply.

**After** — hoist the condition on the signalling thread
(`pc/peer_connection.cc:6203-6208`) and do the work inside the existing batch
(`:6314-6316`). The signature gains `bool enable_sending = false`, so every
other call site is untouched.

**Invariant:** relative order of the two operations is preserved —
`EnableSending` still runs before the pushdown loop.

**Guard:** the folded callee's thread annotation is corrected, not ignored. Here
`RTC_RUN_ON(signaling_thread())` came off `EnableSending`, replaced by a comment
naming the caller (`pc/peer_connection.h:1021-1025`).

---

## P4 — Blocking `Invoke` → `PostTask` plus an idempotent later wait

**Before** — one entry point that posts the teardown and waits for it.

**After** — `video/video_stream_encoder.cc:291-319`: a shared `PostStopTask()`,
an async `StopAsync()` that only posts, and a blocking `Stop()` that posts if
needed then waits. Bodies identical except the trailing wait; `stop_posted_`
makes them mutually idempotent.

**Invariant:** the wait still happens somewhere mandatory. Here
`DestroyChannelInterface` → `~VideoSendStream` → `Stop()` is the join, so no
completion callback is needed. **Look for the wait you already have before
inventing a new one.**

**Guards:** a terminal flag set at the *head* of the posted task (`stopped_`,
`:297`) so work already queued behind the teardown no-ops; the disable posted
before the stop (P5); one shared body, not two copies; the destructor assertion
`RTC_DCHECK(shutdown_event_.Wait(0))` kept as the backstop; the async form
plumbed as a defaulted no-op virtual so non-encoding implementations are
unaffected.

---

## P5 — Sequential teardown → fan out the posts, then wait once

**Before** — destroy unit 1 (wait for its queue) → destroy unit 2 (wait for its
queue) → … Cost is the **sum** of the stops.

**After** — `pc/peer_connection.cc:1148-1164`: disable all, then post all stops,
then the destroy loop, where each `Stop()` now finds `stop_posted_` already true
and only waits. Cost is the **slowest** stop.

**Invariant:** the units must be genuinely independent. Each
`VideoStreamEncoder` owns its own queue, which is what makes the stops
concurrent. Say this explicitly; if the units share a queue there is no win.

**Guards:** the existing per-unit wait is left in place and *becomes* the join —
no new synchronisation is introduced. The disable is fanned out first as a
separate complete pass. Pre-existing ordering constraints are untouched (audio
channels still destroyed after video, because a video channel may hold a pointer
to a voice channel).

---

## P6 — Split one blocking hop into its per-thread halves

So a batch pays each thread once instead of once per element.

**Before** — `BaseChannel::Deinit()` on the worker, containing its own
`network_thread_->Invoke`. For N channels that is N serial rendezvous with the
network thread, all while the signalling thread waits on the worker.

**After** — PR #7's `DetachMediaInterface()` (worker) and `DeinitNetwork_n()`
(network), both idempotent, both exposed on `ChannelInterface`, with `Deinit()`
running whichever has not run. The caller can then do one worker pass and one
network pass for the whole batch.

**Invariant:** each half asserts its own thread and is idempotent, so the old
composed entry point is behaviourally unchanged for a channel torn down the
usual way.

**Guards:** the idempotency flags must be safe for the threads that touch them;
the pointer must be nulled after release; and the wait must be an `Invoke`, not
a condition variable — see the PR #7 section below.

**Status: the split itself was approved in review; the async wrapper around it
was not.**

---

## P7 — Heavy destructor work → a dedicated thread

For an object with **no queue of its own** — a capturer, a source. Fan-out (P5)
does not apply; you need a thread created for the purpose.

**Before** — `~VideoTrack` runs on the signalling thread (the proxy declares
`PROXY_SIGNALING_THREAD_DESTRUCTOR`); dropping the last source reference
destroys the capturer and joins its frame generator thread. `MEASURED at
414-2437 ms per track`, with every other peer's signalling queued behind it.

**After** — `pc/video_track.cc:53-68`:

```cpp
rtc::scoped_refptr<VideoTrackSourceInterface> source = video_source_;
video_source_ = nullptr;
rtc::Thread* const release_thread = VideoSourceReleaseThread();
if (release_thread == nullptr) {
  return;                       // inline fallback, as before
}
release_thread->PostTask(RTC_FROM_HERE, [source = std::move(source)]() mutable {
  source = nullptr;
});
```

**Invariant:** the originating thread must hold **no** remaining reference once
the post is made.

**Guards:** `std::move`, not copy — with `[source]` the destructor's local can
win the race and drop the last reference on the thread you were getting off, so
the optimisation intermittently does not apply. Member nulled before the post.
Thread started once from a function-local static and never joined; it lives for
the process and blocking it costs nothing. Inline fallback if it cannot start.

---

## P8 — Remove per-call proxy machinery with a same-thread fast path

**Before** — `Marshal` constructs the call object *before* anything checks the
thread, so a getter called on the thread it would marshal to still builds a
`ConstMethodCall` and a `SynchronousMethodCall` (the latter holding an
`rtc::Event` by value). Both derive from `rtc::MessageHandler`, and
`~MessageHandler` runs `ThreadManager::Clear`, which takes a process-global lock
and linear-scans every registered thread's queue. Two of those per getter, in
loops over the whole transceiver list on every negotiation.

**After** — `api/proxy.h:293-307` and `:352-366`, two lines added to each of
four macros.

**Invariant — equivalent by construction:** on the same-thread path `Invoke`
already called `proxy_->OnMessage(nullptr)`, the same method on the same object.
`c_` is typed `INTERNAL_CLASS`, so the direct call also devirtualises where the
marshalled path cannot (`RtpTransceiver` is `final`).

**Guards:** zero-argument only, deliberately — no argument forwarding to get
wrong, and those are the ones called in loops. Scope stated numerically: 101
non-test declarations across `api/` and `pc/`, 108 including
`pc/proxy_unittest.cc`. Behind the flag.

**Risk to restate whenever you propose this family:** it alters every proxy
rather than N call sites, and removes a process-global lock acquisition from
every same-thread proxy call. That lock is incidental rather than a designed
barrier, so correct code is unaffected — but it changes interleaving broadly,
and a latent race elsewhere surfaces sooner. Worth a few low-risk nodes first.

---

## P9 — Remove proactive work that has no reader

**Before** — three identical sweeps on the signalling thread, on every local
description, every remote description and every close:

```cpp
// Update stats here so that we have the most recent stats for tracks and
// streams that might be removed by updating the session description.
stats_->UpdateStats(kStatsOutputLevelStandard);
```

**After** — each wrapped in `if (!AmbientFlags::MessageExecutionOptimization())`
(`pc/peer_connection.cc:2685`, `:3140`, `:4613`).

**Invariant:** name the reader and show it refreshes for itself. Here the only
reader is the legacy `GetStats(StatsObserver*, track, level)`, which calls
`stats_->UpdateStats(level)` itself before serving a request.

**Guards:** state exactly what is lost — a post-hoc `getStats` snapshot of
tracks the session update removed, which `StatsCollector` remembers via
`IsValidTrack`. Bound the claimed saving honestly: `UpdateStats` rate-limits
itself to `kMinGatherStatsPeriod`, so the saving is bounded by that period
rather than a full gather per call. Gate rather than delete, so it can be turned
back on — strictly better than commenting it out. Mark any site that was not
separately exercised.

---

## P10 — Bound an unbounded container

**Before** — `transceivers_` is append-only outside `Rollback`. When an m=
section is recycled, `AssociateTransceiver` dissociates the old holder and
nothing removes it. Observed at **4026 entries against 12 m-lines**, and every
walk of the list pays for them.

**After** — `pc/peer_connection.cc:6226-6266`: on reaching stable, if the list
exceeds ten times the m= section count, erase entries that are stopped, hold no
mid, and have no track.

**Invariant:** the erase predicate must make reuse impossible, and each
condition must earn its place.

- `stopped()` — such a transceiver can never send or receive again, and
  `FindFirstTransceiverForAddedTrack` requires `!stopped()`. It also excludes a
  freshly `AddTransceiver`d one the application still holds.
- `!mid()` — erasing one holding a mid would make `GetAssociatedTransceiver(mid)`
  miss and let the m= section associate to a newly created transceiver. Backed
  by measurement: of 473, 462 were stopped and trackless but only 452 were also
  mid-less.

**Guards:** a deliberately loose threshold so a healthy connection pays one size
comparison and no scan. Cheap conditions tested before the `senders()` walk.
Never an `RTC_CHECK`ing accessor in the predicate. Population logged
unconditionally so the ratio is visible before anyone tunes the multiplier.

---

## P11 — Skip work the batch already did, without changing the rest

**Before** — `VideoRtpReceiver::Stop()` always takes its own worker `Invoke`
around `SetSink(nullptr)`, once per receiver.

**After** — `pc/video_rtp_receiver.cc:119-147`: `DetachSinkOnWorker()` does the
work with the caller already on the worker and sets `sink_detached_`; `Stop()`
skips **only** the `Invoke`, while `source_->SetState(kEnded)`,
`delay_->OnStop()` and `stopped_ = true` all still run.

**Invariant:** the original entry point remains correct whether or not the batch
ran. `Stop()` early-returns on `stopped_`, and `SetSink` is idempotent.

**Guards:** the skip flag brackets exactly the skipped statement
(`if (!sink_detached_) { … }`), not the whole method.
`DetachSinkOnWorker()` itself early-returns on `stopped_ || !media_channel_`, so
the batch is a no-op for receivers that do not need it.

---

## D1 — Prefer the primitive-level fix over N hand-converted call sites

**Before** — ten call sites hand-converted from `transceiver->media_type()` to
`transceiver->internal()->media_type()` purely for speed.

**After** — the four zero-argument macros guarded once, and the ten
hand-conversions deleted.

**Invariant — this is the point, not a side effect:** the mechanical fix
*preserves* the safety property; the manual fix *trades it away*. `internal()`
is correct at a call site only while that site provably stays on the signalling
thread, and nothing re-checks that when the code moves.

**Guard:** use `internal()` **only** where the proxy would deadlock — inside a
blocking worker or network `Invoke`. Never as a performance edit.

---

## D2 — Refactor so deferred work has exactly one drain point

**Before** — four error returns each repeating the drain pair:

```cpp
if (!transceiver_or_error.ok()) {
  FlushPendingChannelCreates(&deferred_creates).ok();
  destroy_deferred();
  return transceiver_or_error.MoveError();
}
```

**After** — `pc/peer_connection.cc:3557-3573`: the walk moves into
`UpdateSessionContents`, whose error returns become plain `return`s, and the
caller drains on every exit:

```cpp
RTCError error = UpdateSessionContents(..., &deferred_creates, &deferred_destroys);
RTCError flush_error = FlushPendingChannelCreates(&deferred_creates);
DestroyDeferredChannels(&deferred_destroys);
if (!error.ok()) { return error; }   // the walk's error still wins
return flush_error;
```

**Invariant:** same behaviour — the walk's error still wins over the flush
error, and the queued work still runs on the error paths.

**Guard:** a fifth error return added later cannot forget the drain, because it
does not own it. Deferral transfers ownership to the container, and
`SetChannel(nullptr)` has already dropped the transceiver's reference, so
abandoning it leaks outright rather than merely late.

---

## D3 — Write the gate so the flag-off path is textually upstream

Four techniques, all in the tree:

1. **Deleted code restored under `!flag`,** not left deleted.
2. **Both branches written out in full** rather than sharing a parameterised
   body — `PushdownMediaDescription` (`:6288-6350`) and
   `UpdateTransceiverChannel` (`:3733-3769`) each carry two complete
   implementations, duplicated tails included.
3. **Branch polarity chosen per site** so upstream keeps its original position.
   Both polarities appear; neither is the convention. The convention is
   *upstream fidelity*.
4. **The deferred containers gate the downstream helpers.**
   `DestroyDeferredChannels` and `FlushPendingChannelCreates` have no flag check
   — they early-return on `empty()`, and the vectors are filled only under the
   flag.

**Invariant:** with the flag off the binary behaves exactly as upstream, down to
whether a read goes through the proxy or `internal()`.

**Guard:** if you are factoring out the common part to avoid duplication, you
have probably lost the guarantee. Add a comment at the top of a dual-body
function saying both branches exist — a reviewer who scrolls to the second copy
will otherwise read it as ungated.

---

## The PR #7 anti-pattern: `PostTask` plus a condition variable

PR #7 applied P6 correctly and then wrapped it in a wait that cannot work. It is
**blocked in review on four High and two Medium findings**. Read this before
reaching for any async drain.

**The wrapper** (`DestroyDeferredChannels` becomes a three-hop post chain with a
mutex/CV batch counter, waited on by):

```cpp
void PeerConnection::DrainPendingChannelDestroys() {
  std::unique_lock<std::mutex> lock(pending_destroys_mutex_);
  pending_destroys_cv_.wait(lock, [this] { return pending_destroy_batches_ == 0; });
}
```

**Why it hangs.** `rtc::Thread::Invoke` runs the lambda **inline** when the
target thread is current (`rtc_base/thread.cc:871`) and otherwise pumps the
caller's queue while waiting (`:899`). `PostTask` never runs inline, and
`condition_variable::wait` never pumps. So if signalling is also the worker
thread, hop 1 is queued onto the signalling thread's own queue, signalling then
waits on the CV, the hop never runs, the counter never reaches zero, and `Close`
or the next apply hangs permanently. `BaseChannel` already documents
worker/network aliasing, and factories alias signalling with worker — this is a
supported configuration, not a theoretical one.

**The other findings, each a rule elsewhere in this repo:**

- The drain is installed at `DestroyAllChannels`, `ApplyLocalDescription` and
  `ApplyRemoteDescription` but **not `Rollback()`**. `Rollback` skips channels
  whose transceiver pointer is already null, `RollbackTransports()` then
  destroys the `JsepTransport`, and the queued `DeinitNetwork_n()` disconnects
  from freed memory.
- `SetLocalDescription` / `SetRemoteDescription` now return while the channel
  and encoder are still live, so the observer fires before teardown finishes.
- `network_deinit_done_` is a plain `bool` written on the network thread and
  read on the worker.
- `DisconnectFromRtpTransport()` does not null `rtp_transport_`.

**What it got right, worth copying:** the vector is copied into a local `batch`
before the post and captured by value, and the batch counter is incremented
before the `PostTask`.

**The shape to reach for instead:** one worker `Invoke` containing one nested
network `Invoke` for the whole batch. That still collapses N network rendezvous
into one — the actual goal — without going async at all, and `Invoke` is safe
under aliasing because it degenerates to a direct call. Drop the mutex and the
CV. If a change genuinely must stay async, drain with an `Invoke` of a no-op
rather than a condition variable, and install it at every mutating entry point
including `Rollback()`.

## Related

- `.claude/skills/optimize-message-path/SKILL.md` — the procedure that uses
  these recipes
- `.claude/rules/thread-affinity.md` — deadlock rules every recipe must satisfy
- `.claude/rules/async-teardown.md` — lifetime rules for P4-P7 in particular
- `.claude/rules/ambient-fork.md` — the flag gate for D3
