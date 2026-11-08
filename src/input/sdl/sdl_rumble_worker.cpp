/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "sdl_rumble_worker.h"

#include <cstring>
#include <thread>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_LINUX
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace rex::input::sdl {

namespace {

// SDL clamps to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant.
// XInput vibration holds until the title changes it.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

#if REX_PLATFORM_LINUX
bool IsEvdevPath(const char* path) {
  return path && std::strncmp(path, "/dev/input/event", 16) == 0;
}
#endif

}  // namespace

// A rumble effect uploaded through the worker's own descriptor of a pad's
// evdev node. Without one (not Linux, or the node cannot be opened for
// writing, in which case SDL could not rumble it either) Play fails.
class SDLRumbleWorker::EvdevRumble {
 public:
  explicit EvdevRumble(const char* path) {
#if REX_PLATFORM_LINUX
    descriptor_ = open(path, O_RDWR | O_CLOEXEC);
    effect_.id = -1;
    effect_.type = FF_RUMBLE;
    effect_.replay.length = kRumbleDurationMs;
#else
    (void)path;
#endif
  }
  ~EvdevRumble() {
#if REX_PLATFORM_LINUX
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
#endif
  }
  EvdevRumble(const EvdevRumble&) = delete;
  EvdevRumble& operator=(const EvdevRumble&) = delete;

  // Uploads the speeds and plays them (zero speeds play silence).
  bool Play(const Speeds& speeds) {
#if REX_PLATFORM_LINUX
    if (descriptor_ < 0) {
      return false;
    }
    effect_.u.rumble.strong_magnitude = speeds.low_frequency;
    effect_.u.rumble.weak_magnitude = speeds.high_frequency;
    if (ioctl(descriptor_, EVIOCSFF, &effect_) < 0) {
      return false;
    }
    input_event play{};
    play.type = EV_FF;
    play.code = static_cast<uint16_t>(effect_.id);
    play.value = 1;
    return write(descriptor_, &play, sizeof(play)) == sizeof(play);
#else
    (void)speeds;
    return false;
#endif
  }

 private:
#if REX_PLATFORM_LINUX
  int descriptor_ = -1;
  ff_effect effect_{};
#endif
};

SDLRumbleWorker::SDLRumbleWorker() = default;
SDLRumbleWorker::~SDLRumbleWorker() = default;

void SDLRumbleWorker::Request(SDL_JoystickID pad, const Speeds& speeds) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    auto [requested, added] = requested_.try_emplace(pad, speeds);
    if (!added && requested->second == speeds) {
      return;
    }
    requested->second = speeds;
    pending_[pad] = speeds;
    if (!started_) {
      started_ = true;
      std::thread([self = shared_from_this()] { self->Run(); }).detach();
    }
  }
  wake_.notify_one();
}

void SDLRumbleWorker::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
}

void SDLRumbleWorker::Run() {
  while (true) {
    std::map<SDL_JoystickID, Speeds> batch;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) {
        return;
      }
      batch.swap(pending_);
    }
    for (const auto& [pad, speeds] : batch) {
      ApplyTo(pad, speeds);
    }
  }
}

void SDLRumbleWorker::ApplyTo(SDL_JoystickID pad, const Speeds& speeds) {
#if REX_PLATFORM_LINUX
  const char* path = SDL_GetGamepadPathForID(pad);
  if (IsEvdevPath(path)) {
    auto [rumble, added] = evdev_.try_emplace(pad);
    if (added) {
      rumble->second = std::make_unique<EvdevRumble>(path);
    }
    if (!rumble->second->Play(speeds) && added) {
      REXLOG_WARN("SDL: {} refused a rumble effect", path);
    }
    return;
  }
#endif
  if (SDL_Gamepad* gamepad = SDL_GetGamepadFromID(pad)) {
    SDL_RumbleGamepad(gamepad, speeds.low_frequency, speeds.high_frequency, kRumbleDurationMs);
  }
}

}  // namespace rex::input::sdl
