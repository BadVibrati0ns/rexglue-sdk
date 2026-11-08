/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include <SDL3/SDL.h>

namespace rex::input::sdl {

// Applies controller vibration on a thread of its own, so setting it never
// blocks the guest thread that asked.
//
// Rumbling on the caller (XamInputSetState) froze titles: SDL holds its
// joystick lock across the evdev EVIOCSFF ioctl, and on a uinput pad (the
// virtual controller a remapping daemon such as Steam Input creates) that
// ioctl waits for the daemon to service the force-feedback upload, up to 30 s
// in the kernel. The caller, and every other thread that touched SDL's
// joysticks or the driver's controller mutex, waited with it.
//
// Here a request only records the pad's latest speeds. The worker rumbles
// evdev pads through a descriptor of its own, holding no SDL or driver lock,
// so a daemon that never answers blocks the worker alone; other pads (HIDAPI)
// go through SDL_RumbleGamepad, whose writes do not wait on another process.
class SDLRumbleWorker : public std::enable_shared_from_this<SDLRumbleWorker> {
 public:
  struct Speeds {
    uint16_t low_frequency = 0;   // left motor
    uint16_t high_frequency = 0;  // right motor
    bool operator==(const Speeds&) const = default;
  };

  SDLRumbleWorker();
  ~SDLRumbleWorker();

  // Records the speeds for `pad` and returns at once; the latest request per
  // pad wins. Starts the thread on first use.
  void Request(SDL_JoystickID pad, const Speeds& speeds);
  // Lets the thread exit. It is detached and owns a reference to this object,
  // so a pad stuck in the kernel holds up neither shutdown nor its memory.
  void Stop();

 private:
  void Run();
  void ApplyTo(SDL_JoystickID pad, const Speeds& speeds);

  std::mutex mutex_;  // guards the members up to wake_
  std::map<SDL_JoystickID, Speeds> requested_;
  std::map<SDL_JoystickID, Speeds> pending_;
  bool started_ = false;
  bool stopping_ = false;
  std::condition_variable wake_;

  class EvdevRumble;
  // Worker thread only: one descriptor per evdev pad, by SDL instance ID
  // (never reused, so a pad that reconnects at the same node gets a new one).
  std::map<SDL_JoystickID, std::unique_ptr<EvdevRumble>> evdev_;
};

}  // namespace rex::input::sdl
