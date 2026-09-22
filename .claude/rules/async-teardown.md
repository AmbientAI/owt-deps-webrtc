---
paths:
  - "pc/**/*.cc"
  - "pc/**/*.h"
  - "video/**/*.cc"
  - "video/**/*.h"
  - "media/**/*.cc"
  - "media/**/*.h"
  - "call/**/*.cc"
  - "call/**/*.h"
---

# Async Teardown

Rules for making resource allocation and release asynchronous without releasing
too early, too late, or twice. Read this before turning any blocking teardown
into a posted one.

The deadlock half of the problem lives in `.claude/rules/thread-affinity.md`;
read both before proposing a change. The flag gate is in
`.claude/rules/ambient-fork.md`.

Failure modes in this category are worse than a hang: a use-after-free on a
task queue, a release-build `RTC_CHECK` abort during `Close`, or a media stream
that silently never sends.

## Disable before you release, and post in that order

The most common inversion. The disable is **itself asynchronous** —
`SetSend(false)` posts its own encoder-queue work — so ordering the *posts* is
what orders the *effects*.

Correct sequence, three complete passes, in `pc/peer_connection.cc:1148-1164`:

```cpp
worker_thread()->Invoke<void>(RTC_FROM_HERE, [&] {
  for (cricket::ChannelInterface* channel : video_channels) {
    channel->DisableMedia_w();          // 1. queue the zero-rate update first
  }
  for (cricket::ChannelInterface* channel : video_channels) {
    if (channel->media_channel() != nullptr) {
      static_cast<cricket::VideoMediaChannel*>(channel->media_channel())
          ->StopEncodersAsync();        // 2. then post every teardown
    }
  }
  for (cricket::ChannelInterface* channel : video_channels) {
    DestroyChannelInterface(channel);   // 3. then destroy; each Stop() only waits
  }
  ...
});
```

Post the stops first and `~VideoChannel` → `DisableMedia_w()` → `SetSend(false)`
→ `OnBitrateUpdated(0)` lands on an `encoder_queue_` whose encoder has already
been released.

Do not fan out across units that share ordering constraints. Audio channels are
still destroyed after video here, because a video channel may hold a pointer to
a voice channel — that pre-existing constraint is untouched.

## Guard the queue at the HEAD of the posted teardown

Work already queued behind a teardown must no-op. Set the guard as the **first**
statement of the posted task (`video/video_stream_encoder.cc:291-304`):

```cpp
void VideoStreamEncoder::PostStopTask() {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  stop_posted_ = true;
  video_source_sink_controller_->SetSource(nullptr);
  encoder_queue_.PostTask([this] {
    RTC_DCHECK_RUN_ON(&encoder_queue_);
    stopped_ = true;                      // first statement, not last
    resource_adaptation_processor_->StopResourceAdaptation();
    rate_allocator_ = nullptr;
    bitrate_observer_ = nullptr;
    ReleaseEncoder();
    shutdown_event_.Set();                // completion is a separate signal
  });
}
```

Read by `MaybeEncodeVideoFrame` (`:997`) and the queue-side `OnBitrateUpdated`
(`:1587`), both `RTC_DCHECK_RUN_ON(&encoder_queue_)`.

**Why the head and not the end.** Everything on the queue is serialised, so a
reader can only run strictly before the teardown task or strictly after it —
the flag's value is the same either way for those two cases. The only interval
setting it at the end would change is the one *inside* the teardown task,
spanning `ReleaseEncoder()`, which is exactly the window you need covered.

The flag is not a completion signal. `shutdown_event_` is. Keep them separate.

`ReleaseEncoder()` does not null `encoder_`
(`video/video_stream_encoder.cc:1740-1747`), so this guard is the only thing
stopping a late task from calling into a released encoder.

Three flags, three jobs — do not collapse them:

| Flag | Lives on | Set where | Means |
|---|---|---|---|
| `stop_posted_` | `thread_checker_` (worker) | before the post | a teardown is on the queue |
| `stopped_` | `encoder_queue_` | first statement of the task | nothing after me is valid |
| `shutdown_event_` | cross-thread `rtc::Event` | last statement of the task | the teardown has finished |

## One posted body and one flag for the sync and async forms

Give the async and blocking entry points a single shared post, made mutually
idempotent by one flag (`video/video_stream_encoder.cc:306-319`):

```cpp
void VideoStreamEncoder::StopAsync() {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  if (!stop_posted_) {
    PostStopTask();
  }
}

void VideoStreamEncoder::Stop() {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  if (!stop_posted_) {
    PostStopTask();
  }
  shutdown_event_.Wait(rtc::Event::kForever);
}
```

Identical bodies except the trailing wait. Both are
`RTC_DCHECK_RUN_ON(&thread_checker_)`, so `stop_posted_` needs no atomic.

- **Do not duplicate the teardown body** across the pair. An earlier revision
  did, and the copies immediately started to drift.
- **Prefer an existing mandatory wait over a completion callback.** There is no
  callback here: `DestroyChannelInterface` → `~VideoSendStream` → `Stop()` is
  the join, and the destructor still asserts it ran
  (`RTC_DCHECK(shutdown_event_.Wait(0))`, `:284-287`). Look for the wait you
  already have before inventing a new one.
- Plumb the async form as a **defaulted no-op virtual** so implementations that
  do not encode are unaffected: `api/video/video_stream_encoder_interface.h`,
  `call/video_send_stream.h`, `media/base/media_channel.h:886`.

## Exactly one drain point, reached from every mutating entry

If teardown is fire-and-forget, something must wait for it, in exactly one
place, reached from **every** entry that mutates the state the teardown touches.

Refactor so the drain cannot be forgotten rather than repeating it at each early
return. `pc/peer_connection.cc:3557-3573` moves the walk into a helper whose
error returns are plain `return`s, and drains once in the caller:

```cpp
RTCError error = UpdateSessionContents(..., &deferred_creates, &deferred_destroys);

RTCError flush_error = FlushPendingChannelCreates(&deferred_creates);
DestroyDeferredChannels(&deferred_destroys);

if (!error.ok()) {
  return error;          // the walk's error still wins over the flush error
}
return flush_error;
```

A fifth error return added to the inner function cannot forget the drain,
because it does not own it.

**Deferral transfers ownership to the container.** Abandoning it leaks outright,
not merely late: `SetChannel(nullptr)` at `pc/peer_connection.cc:3735-3739` has
already dropped the transceiver's reference before the push.

**Missing one entry point is a use-after-free, not a leak.** PR #7 drains at
`DestroyAllChannels`, `ApplyLocalDescription` and `ApplyRemoteDescription` but
not `Rollback()`. `Rollback` only destroys a channel whose transceiver pointer
is still set, so it skips the ones already in flight; `RollbackTransports()`
then destroys the `JsepTransport`, `OnTransportChanged` cannot reach the dying
`BaseChannel` to clear its transport, and the still-queued `DeinitNetwork_n()`
disconnects from freed memory.

**The drain must itself be alias-safe.** A `std::condition_variable::wait` on
the signalling thread never pumps, so if signalling is also the worker or
network thread the posted work never runs. Drain with an `Invoke` of a no-op,
which runs inline when the target is current. See
`.claude/rules/thread-affinity.md`.

## Idempotency flags are read only on the thread that writes them

The re-entrancy guards you add to make a split teardown idempotent are
themselves shared state.

```cpp
// RIGHT — guarded by the sequence that writes it.
bool stopped_ RTC_GUARDED_BY(&encoder_queue_) = false;   // video_stream_encoder.h:202

// WRONG — written on the network thread in DeinitNetwork_n, read on the
// worker in Deinit, as a plain bool. The posted chain gives happens-before on
// the happy path; an overlapping teardown (Close / Rollback / destructor) is
// undefined, and can either re-enter the teardown or skip it entirely.
bool network_deinit_done_ = false;                        // PR #7, pc/channel.h
```

If a flag must be read from another thread, make it `std::atomic` or take the
lock, and document in a comment which thread may write it.

A flag that is written and read on one thread only — like
`media_iface_detached_`, worker-only — needs neither.

Changing when teardown can run also invalidates comments elsewhere. PR #7 made
`BaseChannel`'s note on `enabled_` ("can be changed only when signaling thread
does a synchronous call to the worker thread, so it should be safe") false.
Re-read the comments on every member the new path touches.

## Null the handle after releasing it

```cpp
// WRONG — the guard and the pointer can disagree; idempotency rests entirely
// on a separate bool.
if (rtp_transport_) {
  DisconnectFromRtpTransport();
}

// RIGHT
if (rtp_transport_) {
  DisconnectFromRtpTransport();
  rtp_transport_ = nullptr;
}
```

## A destructor that offloads a release must transfer sole ownership

To move work off a thread, the originating thread must hold **no** remaining
reference once the post is made. Null the member, then `std::move` into the
lambda (`pc/video_track.cc:53-68`):

```cpp
rtc::scoped_refptr<VideoTrackSourceInterface> source = video_source_;
video_source_ = nullptr;
rtc::Thread* const release_thread = VideoSourceReleaseThread();
if (release_thread == nullptr) {
  return;                       // inline fallback: release as before
}
release_thread->PostTask(RTC_FROM_HERE, [source = std::move(source)]() mutable {
  source = nullptr;
});
```

With `[source]` instead of `[source = std::move(source)]` the closure and the
local each hold a reference. If the posted task finishes before the destructor's
frame unwinds, **the destructor's local drops the last reference** — on the very
thread the change exists to get off. The optimisation silently, intermittently,
does not apply.

Other requirements for this shape:

- An object with no queue of its own needs a thread created for the purpose.
  Start it once from a function-local static and never join it; it lives for
  the process and blocking it costs nothing.
- Always provide the inline fallback if the thread cannot start.
- Check which thread the destructor runs on before assuming there is a problem:
  the proxy declares it (`PROXY_SIGNALING_THREAD_DESTRUCTOR`, every production
  proxy in this tree).

## `PostTask` capturing `this` needs a guard

This tree has **no** `PendingTaskSafetyFlag` and no `SafeTask`. The guard is
either an `rtc::WeakPtr` or a `scoped_refptr` moved into the lambda.

```cpp
// rtc::WeakPtrFactory, checked inside the task
signaling_thread()->PostTask(
    RTC_FROM_HERE, [self = weak_factory_.GetWeakPtr()] {
      if (self) {
        RTC_DCHECK_RUN_ON(self->signaling_thread());
        self->sctp_data_channels_to_free_.clear();
      }
    });
```

In-tree idioms: `pc/data_channel_controller.cc:360-366`,
`video/video_send_stream_impl.cc:488-495`. A bare `[this]` is acceptable only
for a documented process-lifetime object, or where the task is posted to a queue
the object owns and drains in its own destructor — say which in a comment.

## Making teardown async moves observable completion past the API return

A blocking `Invoke` is often not a performance detail but the guarantee that
teardown finished before the application was told. If a
`SetLocalDescription` / `SetRemoteDescription` observer can now fire while the
encoder, playout, or demuxer registration is still live, say so and say why it
is acceptable. A recycled mid is only safe if the next apply drains first.

PR #6 chose to **delete** a posted deferred-destroy batch rather than defend
this window, once the underlying encoder stop was fast enough to keep the
blocking form.

## Deferral opens an interval where an invariant is false

Name four things, in a comment at the deferral site: the invariant, the
interval, who must not read it, and how a violation would present.
`pc/peer_connection.cc:3561-3566` is the required shape:

```cpp
// Channels for the sections collected in deferred_creates are not attached
// to their transceivers until the flush below. Do not read
// transceiver->internal()->channel() between UpdateSessionContents and
// FlushPendingChannelCreates: PushdownMediaDescription skips a transceiver
// whose channel is null, so a reader added here would surface as a stream
// that never sends rather than as an error.
```

A reader that `continue`s rather than returning an error is the dangerous kind —
the bug presents as missing media with no diagnostic. Where that is possible,
add a detector rather than relying on the comment:

```cpp
if (!channel && content_info && !content_info->rejected) {
  RTC_LOG(LS_ERROR) << "[CONN-DIAG] event=pushdown_no_channel mid="
                    << content_info->name;
}
```

`LS_ERROR` is deliberate: the appliance sets OWT severity to `kError` and drops
`LS_INFO` / `LS_WARNING`, so it is the only level that arrives. It is not a
severity claim.

## Respect the release-build teardown invariants

Any change to destroy order must name the invariant it relies on. These abort in
release, not just in debug:

- `Call::~Call()` `RTC_CHECK`s that every stream map is empty
  (`call/call.cc:479-486`). This is why channels must die before the transports
  and the `Call`.
- `RtpVideoSender::ConfigureSsrcs()` `RTC_CHECK`s that `ssrc_to_rtp_module_` is
  empty on entry (`call/rtp_video_sender.cc:608`) — a *setup* precondition, so
  deferring the matching teardown past a reconfigure aborts here rather than
  where the deferral was introduced.
- `~VideoStreamEncoder` requires `Stop()` first
  (`video/video_stream_encoder.cc:284-287`) — a `RTC_DCHECK`, so debug-only, but
  skipping it means the encoder queue outlives its owner.

## Keep the error-path semantics of the code you replace

Batching changes the number of round trips, not observable behaviour — and
failure semantics are observable behaviour. Match the per-element loop: stop at
the first failure, keep what succeeded, return the same error
(`pc/peer_connection.cc:3670-3719`):

```cpp
// Stops at the first failure, like the per-channel path did: the channels
// created before it are attached below and the error is returned for it.
size_t failed_at = count;
worker_thread()->Invoke<void>(RTC_FROM_HERE, [&] {
  for (size_t i = 0; i < count; ++i) {
    ...
    if (!created[i]) { failed_at = i; break; }
  }
});
for (size_t i = 0; i < failed_at; ++i) { /* attach the prefix */ }
```

Destroying every success on the first failure is new behaviour, not a
requirement of batching. An undo hop is a sign you have changed the contract.

## Removing proactive work requires naming the reader

Before deleting work that nothing obviously consumes, name the reader and show
it refreshes for itself. The legacy stats sweeps were removable because the only
reader, `GetStats(StatsObserver*, track, level)`, calls `stats_->UpdateStats`
itself before serving a request.

State what is lost (here: a post-hoc snapshot of tracks the session update
removed), bound the claimed saving honestly (`UpdateStats` rate-limits itself to
`kMinGatherStatsPeriod`), and gate rather than delete so it can be turned back
on.

## Erasing from a container other code walks

The predicate must make reuse impossible, and each condition must earn its
place. For the transceiver prune (`pc/peer_connection.cc:6235-6266`):

- `stopped()` — such a transceiver can never send or receive again, and
  `FindFirstTransceiverForAddedTrack` requires `!stopped()`. It also excludes a
  freshly `AddTransceiver`d one the application still holds.
- `!mid()` — erasing one holding a mid would make `GetAssociatedTransceiver(mid)`
  miss and let the m= section associate to a newly created transceiver.

Keep the trigger threshold loose so a healthy connection never trips it and pays
only a size comparison, test the cheap conditions before any walk, and log the
population unconditionally so the ratio is visible before anyone tunes the
threshold.

## Related

- `.claude/rules/thread-affinity.md` — which thread work may run on, and the
  alias-safe wait primitives
- `.claude/rules/ambient-fork.md` — the flag gate every change sits behind
- `.claude/skills/optimize-message-path/RECIPES.md` — these rules as
  before/after recipes with the invariant for each
- `.cursor/BUGBOT.md` — the review checklist these rules feed
- `video/video_stream_encoder.cc:284-319` — the reference async-teardown shape
- `pc/video_track.cc:38-68` — the reference destructor-offload shape
