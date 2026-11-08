// Deadline wait adapted from the Project8Recomp runtime patch at c8c21e2781ac.
// Copyright (c) 2026, the Tony Hawk's Project 8 recompilation project contributors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <rex/logging.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::graphics::pg {

// Kept private to the GPU plugin: public SDK headers and the PPC translation
// units need no changes. One owned timer per vblank worker, released on exit.
class DeadlineWait {
 public:
  DeadlineWait() = default;
  DeadlineWait(const DeadlineWait&) = delete;
  DeadlineWait& operator=(const DeadlineWait&) = delete;
  ~DeadlineWait() {
#if REX_PLATFORM_WIN32
    if (timer_) CloseHandle(timer_);
#endif
  }

  void Until(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return;
#if REX_PLATFORM_WIN32
    if (!attempted_) {
      attempted_ = true;
      timer_ = CreateWaitableTimerExW(nullptr, nullptr,
          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    if (timer_) {
      LARGE_INTEGER due;
      due.QuadPart = -std::max<int64_t>(1, ns / 100);
      if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0) &&
          WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0) return;
    }
    // Round up: truncating a sub-millisecond wait to Sleep(0) becomes a spin.
    ::Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
#else
    std::this_thread::sleep_until(deadline);
#endif
  }

 private:
#if REX_PLATFORM_WIN32
  HANDLE timer_ = nullptr;
  bool attempted_ = false;
#endif
};

class VblankStats {
 public:
  explicit VblankStats(bool enabled) : enabled_(enabled) {
    if (enabled_) {
      start_ = std::chrono::steady_clock::now();
      cpu_ = CpuTime();
    }
  }
  void Wake(uint64_t vblanks, bool deadline) {
    if (!enabled_) return;
    ++wakes_;
    vblanks_ += vblanks;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - start_).count();
    if (seconds < 5.0) return;
    const auto cpu_now = CpuTime();
    const double cpu_ms = cpu_ >= 0 && cpu_now >= cpu_ ? double(cpu_now - cpu_) / 10000.0 : -1.0;
    REXLOG_INFO("pg-vblank mode={} wakes_per_s={:.2f} vblanks_per_s={:.2f} cpu_ms={:.3f} window_s={:.3f}",
                deadline ? "deadline" : "legacy", wakes_ / seconds,
                vblanks_ / seconds, cpu_ms, seconds);
    start_ = now;
    cpu_ = cpu_now;
    wakes_ = vblanks_ = 0;
  }

 private:
  static int64_t CpuTime() {
#if REX_PLATFORM_WIN32
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
      const uint64_t k = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
      const uint64_t u = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
      return static_cast<int64_t>(k + u);
    }
#endif
    return -1;
  }
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
  int64_t cpu_ = -1;
  uint64_t wakes_ = 0;
  uint64_t vblanks_ = 0;
};

}  // namespace rex::graphics::pg
