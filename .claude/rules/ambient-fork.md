# Ambient Fork Discipline

This repo is a vendored fork of upstream WebRTC (m88-era) carrying Ambient
patches for Open WebRTC Toolkit. It has no `paths:` frontmatter deliberately:
6672 of the 6690 files are untouched upstream, and the only reason to edit any
of them is an Ambient patch, so this applies every session.

Almost every change here reduces signalling-thread latency on a WebRTC message
path. The topic rules are `.claude/rules/thread-affinity.md` (deadlocks) and
`.claude/rules/async-teardown.md` (resource lifetime).

## There is no CI

Nothing builds or tests this repo on push. Verification is:

```bash
# In the appliance container, against the appliance's libwebrtc pin.
ninja owt            # must produce the full libowt.a
```

then a subscribe/hangup load run against the resulting `webrtc_server`, and an
honest statement in the PR body of which changes that run actually exercised.
Compilation-only coverage is a normal answer — say so rather than implying more.

Do not ask for or add unit tests as a gate; there is no harness here to run
them.

## Everything is behind one flag

`api/ambient_flags.h`, in full:

```cpp
// Process-wide switches set by the embedding application before any
// PeerConnection exists. Header-only so no build file changes are needed.
struct AmbientFlags {
  // Transceiver pruning at stable, no legacy stats sweeps, and the
  // same-thread fast path for zero-argument proxy calls.
  static void SetMessageExecutionOptimization(bool enabled) {
    Flag().store(enabled, std::memory_order_relaxed);
  }
  static bool MessageExecutionOptimization() {
    return Flag().load(std::memory_order_relaxed);
  }

 private:
  static std::atomic<bool>& Flag() {
    static std::atomic<bool> f{false};
    return f;
  }
};
```

Design constraints worth knowing before you add a flag:

- **Header-only, function-local static.** A header under `api/` cannot include
  `system_wrappers/` — `api/DEPS` denies it and `gn check` fails. A `struct`
  with static inline members needs neither a `.cc` nor a `BUILD.gn` edit. An
  earlier revision used `extern "C"` plus an anonymous atomic in
  `pc/peer_connection.cc`, which coupled `api/` down to `pc/` at link time;
  don't go back to that.
- **`memory_order_relaxed` is correct** only because of the documented contract:
  set by the embedding application before any `PeerConnection` exists. Nothing
  is published through the flag.
- **Defaults `false`,** and the appliance sets it from `NodeConfig
  webrtc_message_execution_optimization`.
- **One flag for several PRs is deliberate** — they are turned on together
  atomically. This was raised in review and accepted. Add a `v2` rather than
  splitting this one.

Includers, all three: `api/proxy.h:65`, `pc/peer_connection.cc:23`,
`pc/video_track.cc:18`.

## The flag-off path must be textually upstream

The governing convention is **upstream fidelity**, not a fixed branch polarity.
Pick whichever shape leaves the upstream body verbatim and, where practical, in
its original position.

**Restoring deleted work** — negated gate around the upstream statement:

```cpp
if (!AmbientFlags::MessageExecutionOptimization()) {
  stats_->UpdateStats(kStatsOutputLevelStandard);
}
```

**Replacing a whole body** — early return with upstream inline, duplicated tail
and all (`pc/peer_connection.cc:1114-1131`):

```cpp
void PeerConnection::DestroyAllChannels() {
  if (!AmbientFlags::MessageExecutionOptimization()) {
    ...upstream loops, byte for byte...
    DestroyDataChannelTransport();
    return;
  }
  ...new code, which repeats DestroyDataChannelTransport() at the end...
```

**Inverted polarity** where that keeps upstream at the bottom of the function
(`pc/peer_connection.cc:3733-3769`): flag-on is the early return, upstream is
the fall-through.

Two further rules:

- **Write both branches out in full.** `PushdownMediaDescription`
  (`:6288-6350`) and `UpdateTransceiverChannel` (`:3733-3769`) each carry two
  complete implementations rather than sharing a parameterised body. If you
  find yourself factoring out the common part to avoid duplication, you have
  probably lost the guarantee. This codebase duplicates deliberately.
- **Fidelity extends to individual reads.** `internal()` is safe and cheaper
  than the proxy even with the flag off, but it is not what upstream does, so
  the Plan B read at `:4074-4084` is gated too. Off, every site is upstream's.

**Downstream helpers may gate on emptiness instead.**
`DestroyDeferredChannels` and `FlushPendingChannelCreates` contain no flag check
at all — they early-return on `empty()`, and the vectors are only ever filled
under the flag at `:3734`.

When both branches are written out, add a one-line comment at the top of the
function saying so. A reviewer who scrolls to the second copy will otherwise
read it as ungated; that has already happened once in review.

### Gate sites

Sixteen today. If you add one, keep this list current.

| File | Lines | Gates |
|---|---|---|
| `api/proxy.h` | 295, 303, 354, 362 | same-thread fast path in `PROXY_METHOD0`, `PROXY_CONSTMETHOD0`, `PROXY_WORKER_METHOD0`, `PROXY_WORKER_CONSTMETHOD0` |
| `pc/peer_connection.cc` | 1115 | `DestroyAllChannels`: batched detach + one worker hop |
| | 2685, 3140, 4613 | legacy `stats_->UpdateStats` sweeps on apply-local, apply-remote, `Close` |
| | 3734 | `UpdateTransceiverChannel`: deferred create / destroy path |
| | 4077 | `FindMediaSectionForTransceiver`: Plan B reads through `internal()` |
| | 4620 | `Close`: one worker hop to detach every video receiver sink |
| | 6206 | `EnableSending` folded into the pushdown batch |
| | 6235, 6257 | transceiver prune trigger and its diagnostic log |
| | 6289 | `PushdownMediaDescription`: one worker hop for the whole apply |
| `pc/video_track.cc` | 55 | `~VideoTrack`: release the source off the signalling thread |

Not gated, deliberately: the `StopAsync` / `StopEncoderAsync` /
`StopEncodersAsync` virtuals. They default to no-ops and have no caller outside
the flag-on `DestroyAllChannels` branch, so they are dead code when off.

## Ambient-added symbols

Everything below is ours, not upstream's. Add to this table when you add
another. These symbols span 17 files; `api/proxy.h` is the eighteenth
Ambient-touched file and is covered by the gate-site table above.

To check this table against the tree — for instance after an upstream roll:

```bash
git diff --name-status 18721dffbee8 HEAD    # last pre-Ambient commit
```

| Symbol | Location |
|---|---|
| `AmbientFlags` | `api/ambient_flags.h` |
| `VideoStreamEncoder::StopAsync`, `PostStopTask`, `stop_posted_`, `stopped_` | `video/video_stream_encoder.h:88`, `:194`, `:201-202`; `.cc:291-319` |
| `VideoStreamEncoderInterface::StopAsync` (no-op default) | `api/video/video_stream_encoder_interface.h:122` |
| `VideoSendStream::StopEncoderAsync` | `call/video_send_stream.h:219` (no-op default); `video/video_send_stream.h:90`; `video/video_send_stream.cc:210-213` |
| `VideoMediaChannel::StopEncodersAsync` | `media/base/media_channel.h:886` (no-op default); `media/engine/webrtc_video_engine.h:139`; `.cc:641` |
| `WebRtcVideoSendStream::StopEncoderAsync` | `media/engine/webrtc_video_engine.h:361` |
| `ChannelInterface::DisableMedia_w` (promoted to a virtual) | `pc/channel_interface.h:67`; `pc/channel.h:102` |
| `VideoRtpReceiver::DetachSinkOnWorker`, `sink_detached_` | `pc/video_rtp_receiver.h:92`, `:137`; `.cc:119-125` |
| `PeerConnection::DestroyAllChannels` (rewritten) | `pc/peer_connection.h:1028`; `.cc:1114-1167` |
| `PeerConnection::DestroyDeferredChannels`, `FlushPendingChannelCreates`, `PendingChannelCreate` | `pc/peer_connection.h:630-661`; `.cc:3635-3720` |
| `PeerConnection::PushdownMediaDescription(…, bool enable_sending)` | `pc/peer_connection.cc:6289` |
| transceiver prune at stable | `pc/peer_connection.cc:6226-6266` |
| `VideoSourceReleaseThread`, async `~VideoTrack` | `pc/video_track.cc:38-68` |

## Commit and PR conventions

Commit subjects are imperative and under ~72 characters. The body states cost,
then mechanism, then guard, then measurement — a miniature version of the PR
template. Honest negatives belong in the commit message too ("No caller yet.").

Credit review provenance inline: `Reported by Bugbot on #4.`,
`Suggested by aditya-ambient in review.`

The PR body shape, and the `MEASURED` / `INFERRED` evidence tags it uses, are
specified in `.claude/skills/optimize-message-path/SKILL.md`. Follow it for any
change in this theme.

## Related

- `.claude/rules/thread-affinity.md` — deadlock rules
- `.claude/rules/async-teardown.md` — resource lifetime rules
- `.claude/skills/optimize-message-path/SKILL.md` — how to find and propose one
  of these changes, and the PR write-up template
- `.cursor/BUGBOT.md` — the review policy Bugbot applies to every PR
- `CLAUDE.md` — repo orientation and the conventions table
