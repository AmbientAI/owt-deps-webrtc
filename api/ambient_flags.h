/*
 *  Copyright 2026 Ambient.ai. All Rights Reserved.
 */

#ifndef API_AMBIENT_FLAGS_H_
#define API_AMBIENT_FLAGS_H_

#include <atomic>

namespace webrtc {

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

}  // namespace webrtc

#endif  // API_AMBIENT_FLAGS_H_
