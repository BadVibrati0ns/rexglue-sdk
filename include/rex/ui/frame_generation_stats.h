/**
 * @file        rex/ui/frame_generation_stats.h
 *
 * @brief       Live FSR 3 frame generation figures for debug overlays.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

namespace rex::ui {

struct FrameGenerationStats {
  // The frame generation swapchain exists (fsr3_frame_generation at startup).
  bool available = false;
  // The last painted frame gave FSR 3 the image without the HUD.
  bool hudless = false;
  // Averaged over about half a second.
  double painted_fps = 0;
  double presented_fps = 0;
};

FrameGenerationStats GetFrameGenerationStats();

}  // namespace rex::ui
