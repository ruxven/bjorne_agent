#include "platform/Shutdown.h"

#include <csignal>

#include <catch_amalgamated.hpp>

using bjorne_agent::platform::clearShutdownRequest;
using bjorne_agent::platform::installShutdownHandlers;
using bjorne_agent::platform::shutdownRequested;

TEST_CASE("a shutdown request is not there at first") {
  clearShutdownRequest();
  REQUIRE_FALSE(shutdownRequested());
}

TEST_CASE("the shutdown handlers install themselves") {
  REQUIRE(installShutdownHandlers());
  clearShutdownRequest();
  REQUIRE_FALSE(shutdownRequested());
}

TEST_CASE("a signal sets the shutdown flag") {
  clearShutdownRequest();
  REQUIRE_FALSE(shutdownRequested());

  // The handler does nothing but set the flag, so a test can call it the way
  // the operating system would.
  bjorne_agent::platform::detail::handleShutdownSignal(SIGINT);
  REQUIRE(shutdownRequested());

  // The flag is the same one the handlers write to.
  REQUIRE(bjorne_agent::platform::detail::shutdownFlag() != 0);

  clearShutdownRequest();
  REQUIRE_FALSE(shutdownRequested());
}
