#include <chrono>
#include <csignal>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

// The private child receives no inherited credentials or broker configuration.
TEST(FleetIntakeStartup, IncompleteConfigurationExitsBeforeServingLegacyFallback) {
  std::string executable = NESTOR_TEST_SERVER;
  char auth[] = "NESTOR_AUTH_TOKEN=fixture-key";
  char mode[] = "NESTOR_AUTH_MODE=required";
  char repository[] = "NESTOR_FLEET_STATE_REPOSITORY=private/state";
  char broker[] = "NATS_URL=nats://127.0.0.1:1";
  char host[] = "NESTOR_BIND_ADDR=127.0.0.1";
  char port[] = "NESTOR_PORT=0";
  char* environment[] = {auth, mode, repository, broker, host, port, nullptr};
  char* arguments[] = {executable.data(), nullptr};
  pid_t child = 0;
  ASSERT_EQ(posix_spawn(&child, executable.c_str(), nullptr, nullptr, arguments, environment), 0);
  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child) {
      exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!exited) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
  }
  EXPECT_TRUE(exited) << "Incomplete Fleet configuration must not serve a legacy fallback";
  EXPECT_TRUE(WIFEXITED(status));
  if (WIFEXITED(status)) EXPECT_EQ(WEXITSTATUS(status), 1);
}
