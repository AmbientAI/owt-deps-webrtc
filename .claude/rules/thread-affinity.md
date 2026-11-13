---
paths:
  - "api/**/*.cc"
  - "api/**/*.h"
  - "pc/**/*.cc"
  - "pc/**/*.h"
  - "media/**/*.cc"
  - "media/**/*.h"
  - "video/**/*.cc"
  - "video/**/*.h"
  - "call/**/*.cc"
  - "call/**/*.h"
  - "p2p/**/*.cc"
  - "p2p/**/*.h"
  - "rtc_base/**/*.cc"
  - "rtc_base/**/*.h"
---

# Thread Affinity

Rules for deciding which thread work runs on, and for not deadlocking when you
move it. The failure mode in this category is a **release-build hang of two
threads with no log line and no test coverage** — every one of these bugs was
either caught in review on #3/#4/#6/#7 or is live on #7 today.

Resource allocation and release ordering is the other half of the problem and
lives in `.claude/rules/async-teardown.md`. The feature-flag discipline that
gates all of it is in `.claude/rules/ambient-fork.md`.

## Read these three pieces of code first

Almost every bug in this category follows from one of them.

**1. The proxy macros — the fast path tests the TARGET thread**
(`api/proxy.h:293-307`):

```cpp
#define PROXY_METHOD0(r, method)                           \
  r method() override {                                    \
    if (AmbientFlags::MessageExecutionOptimization() && signaling_thread_->IsCurrent()) \
      return c_->method();                                 \
    MethodCall<C, r> call(c_, &C::method);                 \
    return call.Marshal(RTC_FROM_HERE, signaling_thread_); \
  }
```

Called from the worker thread, `signaling_thread_->IsCurrent()` is `false`, so
this takes the marshal path regardless of the flag. `PROXY_METHOD1` through
`PROXY_METHOD5`, and every argument-taking worker variant, have no fast path at
all.

**2. What the transceiver proxy marshals** (`pc/rtp_transceiver.h:226-244`) —
thirteen methods and the destructor, all to the **signalling** thread:

```cpp
BEGIN_SIGNALING_PROXY_MAP(RtpTransceiver)
PROXY_SIGNALING_THREAD_DESTRUCTOR()
PROXY_CONSTMETHOD0(cricket::MediaType, media_type)
PROXY_CONSTMETHOD0(absl::optional<std::string>, mid)
PROXY_CONSTMETHOD0(rtc::scoped_refptr<RtpSenderInterface>, sender)
PROXY_CONSTMETHOD0(rtc::scoped_refptr<RtpReceiverInterface>, receiver)
PROXY_CONSTMETHOD0(bool, stopped)
...
```

**3. The two accessors that abort in release** (`pc/rtp_transceiver.cc:219-230`):

```cpp
rtc::scoped_refptr<RtpSenderInternal> RtpTransceiver::sender_internal() const {
  RTC_DCHECK(unified_plan_);
  RTC_CHECK_EQ(1u, senders_.size());
  return senders_[0]->internal();
}
```

`RTC_DCHECK` compiles out in release. `RTC_CHECK_EQ` does not — it aborts the
process.

## The thread map

| Thread / queue | Accessor | Owns |
|---|---|---|
| signalling | `signaling_thread()` (`pc/peer_connection.h:252`) | SDP, transceivers, proxies, most `PeerConnection` state |
| worker | `worker_thread()` (`:251`) | channel media, `*_w` methods, encoder lifecycle, `Call` |
| network | `network_thread()` (`:248`) | ICE, DTLS, RTP transport, SCTP, `*_n` methods |
| `encoder_queue_` | `video/video_stream_encoder.h:418` | one per `VideoStreamEncoder`; encode + rate updates |
| `worker_queue_` | `video/video_send_stream_impl.h:157` | send-stream state |
| `task_queue_` | `call/rtp_transport_controller_send.cc:117` | `rtp_send_controller` |
| `"ModuleProcessThread"` / `"PacerThread"` | `call/call.cc:404-405` | module and pacer ticks |
| `"VideoSourceRelease"` | `pc/video_track.cc:39-49` | Ambient-added; process-lifetime, never joined |

Method-name suffixes are load-bearing: `_w` is worker, `_n` is network, `_s` is
signalling. Honour them when adding a method.

**Signalling, worker and network may be the same `rtc::Thread`.** `BaseChannel`
documents worker/network aliasing, and factories alias signalling with worker.
`Thread::Send` tolerates it by short-circuiting; anything you write must too.

## Blocking versus posting

| Primitive | Blocks? | Short-circuits when target is current? | Pumps the caller's queue while waiting? |
|---|---|---|---|
| `rtc::Thread::Invoke<T>` / `Send` | yes | **yes** (`rtc_base/thread.cc:871`) | **yes** (`:899`) |
| proxy `Marshal` → `SynchronousMethodCall::Invoke` | yes | yes (`api/proxy.cc:23`) | no — `Event::Wait(kForever)` |
| `std::condition_variable::wait` | yes | no | no |
| bare `rtc::Event::Wait` | yes | no | no |
| `Thread::Join`, `std::future::get` | yes | no | no |
| `Thread::PostTask` / `TaskQueueBase::PostTask` | no | n/a (always queues) | n/a |

`Thread::Send` is the only wait in this tree that is safe under thread
aliasing, because of the first two columns:

```cpp
  if (IsCurrent()) {
    msg.phandler->OnMessage(&msg);
    return;
  }
  ...
  while (!ready) {
    crit_.Leave();
    current_thread->socketserver()->Wait(kForever, false);
```

Note that pumping the queue does **not** make it safe to call back into the
blocked thread synchronously: `socketserver()->Wait(kForever, false)` handles
socket events, it does not dispatch the queued `Send`.

## Never call a signalling proxy from inside a blocking worker `Invoke`

This is the single most important rule in this file, and the one most commonly
reintroduced by plausible-looking refactors.

A blocking `worker_thread()->Invoke` parks the signalling thread. Any
synchronous call back to signalling from inside the lambda hangs both threads
permanently. Use `internal()` there — it returns the same value.

```cpp
// WRONG — media_type() is PROXY_CONSTMETHOD0 under BEGIN_SIGNALING_PROXY_MAP.
// From the worker it marshals back to the signalling thread that is blocked
// in this very Invoke.
worker_thread()->Invoke<void>(RTC_FROM_HERE, [&] {
  for (const auto& transceiver : transceivers_) {
    if (transceiver->media_type() == cricket::MEDIA_TYPE_VIDEO) { ... }
  }
});

// RIGHT
worker_thread()->Invoke<void>(RTC_FROM_HERE, [&] {
  for (const auto& transceiver : transceivers_) {
    if (transceiver->internal()->media_type() == cricket::MEDIA_TYPE_VIDEO) { ... }
  }
});
```

Reference sites: `pc/peer_connection.cc:4079` (the flag-off proxy read) against
`:4083` (the `internal()` read); `:1142`; `:4626-4635`.

This is a **correctness** requirement, not an optimisation. See "Fix the
primitive, not the call site" below for why you must not use `internal()`
anywhere else.

## Audit a relocated statement by its callee graph

When you move a block to another thread, most of it usually already ran there.
The bug is in the one callee that did not. The PR must name them.

The canonical example: moving `DestroyTransceiverChannel` into a worker
`Invoke` looks safe, because `ChannelManager::DestroyVoiceChannel` /
`DestroyVideoChannel` always did their own `Invoke`. The only newly relocated
code is `RtpTransceiver::SetChannel(nullptr)`, and exactly one of its callees is
a problem — `VideoRtpReceiver::Stop()` (`pc/video_rtp_receiver.cc:127-147`),
which makes two blocking hops back to signalling:

- `delay_->OnStop()` — `PROXY_METHOD0` on `JitterBufferDelayProxy`
  (`pc/jitter_buffer_delay_proxy.h:25`).
- `source_->SetState(kEnded)` → `FireOnChanged()` → `VideoTrack::OnChanged()` →
  `video_source_->state()` — `PROXY_CONSTMETHOD0` on `VideoTrackSourceProxy`
  (`api/video_track_source_proxy.h:25`).

The resolution was to keep the detach on the signalling thread and relocate only
the destroy (`pc/peer_connection.cc:1131-1164`), with a comment recording why so
a later refactor does not undo it.

## Safety must be structural, not incidental

A hop back to the blocked thread that is unreachable *today* is still a bug if
nothing in the code enforces the reachability argument.

`VideoRtpReceiver::Stop()` above is dead code on every current path only because
`stopped_` is initialised `true` and cleared only by `RestartMediaChannel()`,
and because both callers happen to run the `transceiver->Stop()` loop first.
Reordering `Close()`, adding a third call site, or landing a receiver that gets
re-set-up late turns it into a silent two-thread hang.

Prefer restructuring so the unsafe call cannot be reached. A comment at the
function plus a comment at every call site is the fallback, not the fix.

## `Invoke` may capture by reference; `PostTask` must not

A blocking `Invoke` runs before the caller's frame unwinds, so `[&]` is fine
(`pc/peer_connection.cc:3673`). A `PostTask` outlives the frame: copy into a
local first and capture that by value.

```cpp
// The batch is copied out of the caller's vector before the post.
const std::vector<cricket::ChannelInterface*> batch(channels->begin(),
                                                    channels->end());
channels->clear();
worker_thread()->PostTask(RTC_FROM_HERE, [this, batch] { ... });
```

Capturing `this` needs its own guard — see `.claude/rules/async-teardown.md`.

## Walk `senders()` / `receivers()`, not the `_internal()` accessors

`sender_internal()` and `receiver_internal()` are Unified-Plan conveniences with
an `RTC_CHECK_EQ(1u, …)` precondition, and Plan B leaves a transceiver with none
or several. Never use them on a path Plan B can reach.

```cpp
// WRONG — aborts in release under Plan B, and the null guard below it can
// never run because the RTC_CHECK fires first.
auto receiver = transceiver->internal()->receiver_internal();
if (!receiver) { continue; }

// RIGHT
for (const auto& receiver : transceiver->internal()->receivers()) {
  RtpReceiverInternal* const internal = receiver->internal();
  if (internal == nullptr) { continue; }
  ...
}
```

Reference: `pc/peer_connection.cc:4626-4635` (receivers), `:6243-6253`
(senders). Test the cheap conditions before the walk so it only runs for
candidates.

## Thread annotations

- `RTC_RUN_ON(x)` on a declaration, `RTC_DCHECK_RUN_ON(x)` at the top of the
  body, `RTC_GUARDED_BY(x)` on a member
  (`rtc_base/synchronization/sequence_checker.h:162-167`).
- **`RTC_DCHECK_RUN_ON` compiles out in release.** A wrong annotation is silent
  in production and gives false confidence in review. `RTC_CHECK` and `FATAL()`
  (`rtc_base/checks.h:370`, `:434`) abort in every build.
- Moving a body to a different thread means **moving** the annotation, not
  deleting it. If the function genuinely runs on either thread, say so in a
  comment naming the callers, as `EnableSending` does
  (`pc/peer_connection.h:1021-1025`).
- If you cannot state which thread a new function runs on, the design is not
  finished yet.

## Fix the primitive, not the call site

When a threading cost appears at many call sites, fix the primitive. PR #3
replaced ten hand-converted `internal()` call sites with two lines in each of
four macros, covering every zero-argument proxy declaration in the tree — 101
outside tests.

The reason is safety, not reach: converting a call site to `internal()` is
correct only while that site provably stays on the signalling thread, and
nothing re-checks that when the code moves. Guarding the macro keeps proxy
semantics and skips the machinery only when it is already on the right thread.

So: use `internal()` **only** where the proxy would deadlock — inside a blocking
worker or network `Invoke`. Never as a performance edit.

If you do change a primitive, state the blast radius. Removing the
`~MessageHandler` → `ThreadManager::Clear` sweep from every same-thread proxy
call also removes a process-global lock acquisition, which changes interleaving
broadly; correct code is unaffected but a latent race elsewhere surfaces sooner.

## Related

- `.claude/rules/async-teardown.md` — resource release ordering, queue-side
  guards, `PostTask` capture safety
- `.claude/rules/ambient-fork.md` — the flag gate every change sits behind
- `.claude/skills/optimize-message-path/SKILL.md` — the procedure for finding
  and proposing a thread-hop change
- `.cursor/BUGBOT.md` — the review checklist these rules feed
- `rtc_base/thread.cc:855-918` — `Thread::Send`, the only alias-safe wait
- `pc/peer_connection.cc:1114-1167` — `DestroyAllChannels`, the worked example
  for splitting a batch across the signalling/worker boundary
