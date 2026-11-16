// A Catch2 listener that names a test case that has been running too long, and
// that keeps the report readable across a crash.
//
// The suite drives threads and child processes, so a wedged wait shows up as a
// CI job that sits there until the harness kills it, with nothing in the log
// saying which case caused it. This listener turns that into a message that
// names the case and then fails the run. The per case flush is for the other
// way a run dies: a crash throws away whatever the report had buffered, and
// the CI shows only the log tail. The optional trace names each case on
// stderr as it starts, which is the only record left when a case kills the
// process before the report can say anything.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <catch_amalgamated.hpp>

namespace {

// The whole suite finishes in seconds on a healthy machine, so a case that runs
// for minutes is wedged. The environment variable exists so the watchdog can be
// exercised without waiting two minutes.
constexpr int kDefaultLimitSeconds = 120;

struct Watch {
  std::mutex mutex;
  std::string current;
  std::chrono::steady_clock::time_point started;
  bool running = false;
};

// Deliberately leaked: the watcher thread is never joined, and a static object
// would be destroyed under it while the process exits.
Watch& watch() {
  static Watch* instance = new Watch();
  return *instance;
}

int limitSeconds() {
  const char* value = std::getenv("BJORNE_AGENT_TEST_WATCHDOG_SECONDS");
  if (value == nullptr) {
    return kDefaultLimitSeconds;
  }
  const int seconds = std::atoi(value);
  return seconds > 0 ? seconds : kDefaultLimitSeconds;
}

// Set on a runner whose test step keeps no readable log. Every case names
// itself as it starts, on unbuffered stderr, so the last line points at the
// case that took the process down even when the report itself is lost.
bool tracing() {
  const char* value = std::getenv("BJORNE_AGENT_TEST_TRACE");
  return value != nullptr && std::string(value) == "1";
}

void traceCase(const std::string& name) {
  std::fprintf(stderr, "--- %s\n", name.c_str());
  std::fflush(stderr);
}

void watchLoop() {
  const auto limit = std::chrono::seconds(limitSeconds());
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    Watch& state = watch();
    std::string name;
    {
      std::lock_guard<std::mutex> guard(state.mutex);
      if (!state.running || std::chrono::steady_clock::now() - state.started < limit) {
        continue;
      }
      name = state.current;
    }
    // A workflow command as well as a plain line: the runner turns the first
    // into an annotation, which stays readable after the job log has been
    // rolled up, so the culprit can be found from the check run alone.
    std::fprintf(stdout, "\n::error title=Hanging test::The test case \"%s\" has been running for more than %lld seconds.\n",
                 name.c_str(), static_cast<long long>(limit.count()));
    std::fprintf(stderr, "\nThe test case \"%s\" has been running for more than %lld seconds.\n", name.c_str(),
                 static_cast<long long>(limit.count()));
    std::fflush(stdout);
    std::fflush(stderr);
    // _Exit rather than abort: no atexit handlers and no debugger break, so the
    // run fails cleanly and the harness sees a non-zero status.
    std::_Exit(1);
  }
}

class TestWatchdog final : public Catch::EventListenerBase {
 public:
  explicit TestWatchdog(const Catch::IConfig* config) : Catch::EventListenerBase(config) {}

  void testCaseStarting(const Catch::TestCaseInfo& info) override {
    if (tracing()) {
      traceCase(info.name);
    }
    Watch& state = watch();
    std::lock_guard<std::mutex> guard(state.mutex);
    state.current = info.name;
    state.started = std::chrono::steady_clock::now();
    state.running = true;
    if (!thread_started_) {
      thread_started_ = true;
      std::thread(watchLoop).detach();
    }
  }

  void testCaseEnded(const Catch::TestCaseStats&) override {
    // A crash would otherwise take the buffered report with it, and the CI only
    // shows the tail of the log.
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);

    Watch& state = watch();
    std::lock_guard<std::mutex> guard(state.mutex);
    state.running = false;
  }

 private:
  bool thread_started_ = false;
};

}  // namespace

CATCH_REGISTER_LISTENER(TestWatchdog)
