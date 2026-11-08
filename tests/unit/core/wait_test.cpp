/**
 * @file        tests/unit/core/wait_test.cpp
 * @brief       Unit tests for timed and alertable waits (rex::thread::Wait,
 *              WaitMultiple).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>
#include <rex/thread.h>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <time.h>
#endif

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
// CPU time the calling thread has used.
std::chrono::nanoseconds ThreadCpuTime() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
}
#endif

}  // namespace

TEST_CASE("WaitMultiple times out no earlier than its timeout", "[thread][wait]") {
  auto first = rex::thread::Event::CreateManualResetEvent(false);
  auto second = rex::thread::Event::CreateManualResetEvent(false);
  rex::thread::WaitHandle* handles[] = {first.get(), second.get()};
  for (bool alertable : {false, true}) {
    const auto start = Clock::now();
    auto result = rex::thread::WaitMultiple(handles, 2, false, alertable, 15ms);
    CHECK(result.first == rex::thread::WaitResult::kTimeout);
    CHECK(Clock::now() - start >= 15ms);
  }
}

TEST_CASE("WaitMultiple returns the signaled handle", "[thread][wait]") {
  auto first = rex::thread::Event::CreateManualResetEvent(false);
  auto second = rex::thread::Event::CreateManualResetEvent(false);
  rex::thread::WaitHandle* handles[] = {first.get(), second.get()};
  std::thread signaler([&] {
    std::this_thread::sleep_for(5ms);
    second->Set();
  });
  auto result = rex::thread::WaitMultiple(handles, 2, false, false, 1000ms);
  signaler.join();
  CHECK(result.first == rex::thread::WaitResult::kSuccess);
  CHECK(result.second == 1);
}

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
// Remaining time used to be truncated to whole milliseconds, so the last
// partial millisecond of each wait, and every 1 ms slice of an alertable
// one, was a zero-length sleep: a busy spin for the whole alertable wait.
TEST_CASE("Alertable waits sleep instead of spinning", "[thread][wait]") {
  auto first = rex::thread::Event::CreateManualResetEvent(false);
  auto second = rex::thread::Event::CreateManualResetEvent(false);
  rex::thread::WaitHandle* handles[] = {first.get(), second.get()};
  const auto cpu_start = ThreadCpuTime();
  rex::thread::WaitMultiple(handles, 2, false, true, 50ms);
  rex::thread::Wait(first.get(), true, 50ms);
  CHECK(ThreadCpuTime() - cpu_start < 25ms);
}
#endif
