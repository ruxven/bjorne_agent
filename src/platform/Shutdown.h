#pragma once

#include <csignal>

namespace bjorne_agent {
namespace platform {

namespace detail {

// The handler runs inside a signal, so all it may do is set this flag. The
// agent loop polls it between steps and persists the session there, where the
// normal allocator and file code is safe to use.
inline volatile std::sig_atomic_t& shutdownFlag() {
  static volatile std::sig_atomic_t flag = 0;
  return flag;
}

inline void handleShutdownSignal(int) { shutdownFlag() = 1; }

}  // namespace detail

// Installs handlers for SIGINT and SIGTERM. The process keeps running until the
// agent loop observes the flag and saves the session, so a container stop does
// not lose the turn that already finished.
inline void installShutdownHandlers() {
  std::signal(SIGINT, detail::handleShutdownSignal);
#ifdef SIGTERM
  std::signal(SIGTERM, detail::handleShutdownSignal);
#endif
}

inline bool shutdownRequested() { return detail::shutdownFlag() != 0; }

// Clears the flag. Only a test or a long lived embedding needs this; the
// process exits after a shutdown request in the normal case.
inline void clearShutdownRequest() { detail::shutdownFlag() = 0; }

}  // namespace platform
}  // namespace bjorne_agent
