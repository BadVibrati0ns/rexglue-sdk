/**
 * @file        tests/unit/core/timer_queue_test.cpp
 * @brief       Unit tests for the timer queue's dispatch thread
 *              (rex::thread::QueueTimerOnce).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <rex/thread/timer_queue.h>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("Timer queue wakes for a timer queued while it sleeps", "[thread][timer]") {
  std::atomic<bool> far_fired{false};
  std::atomic<bool> fired{false};
  // Leave the dispatch thread waiting for a timer an hour away, then queue an
  // earlier one: only a wake-up on publish gets it out of that wait in time.
  auto far = rex::thread::QueueTimerOnce([&](void*) { far_fired = true; }, nullptr,
                                         Clock::now() + std::chrono::hours(1));
  std::this_thread::sleep_for(20ms);
  const auto start = Clock::now();
  auto near = rex::thread::QueueTimerOnce([&](void*) { fired = true; }, nullptr, start + 5ms);
  while (!fired && Clock::now() - start < 2s) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(fired);
  CHECK(Clock::now() - start < 500ms);
  CHECK_FALSE(far_fired);
  if (auto item = far.lock()) {
    item->Disarm();
  }
}
