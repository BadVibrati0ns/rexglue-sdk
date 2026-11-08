/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

namespace rex {
namespace ui {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
#if REX_PLATFORM_MAC
  // The vendored RenderDoc app API header doesn't expose a macOS path.
  // Keep RenderDoc optional and simply report "not connected" on this platform.
  return nullptr;
#else
  std::unique_ptr<RenderDocAPI> renderdoc_api(new RenderDocAPI());

  pRENDERDOC_GetAPI get_api = nullptr;

  // An attached RenderDoc (launched through it, or injected) has already
  // loaded its library into the process. Only look for it there: loading it
  // would also succeed wherever RenderDoc is merely installed (SteamOS ships
  // it system-wide), and RenderDoc would then wrap every Vulkan call and turn
  // on GPU debug markers with nothing attached (a Steam Deck frame took 9.1
  // instead of 5.3 ms).
  if (!renderdoc_api->library_.LoadIfAlreadyLoaded(platform::lib_names::kRenderDoc)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  // get_api will be null if RenderDoc is not connected, or the API isn't
  // available on this platform, or there was an error.
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, (void**)&renderdoc_api->api_1_0_0_) ||
      !renderdoc_api->api_1_0_0_) {
    return nullptr;
  }

  REXLOG_INFO("RenderDoc API initialized");

  return renderdoc_api;
#endif
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace ui
}  // namespace rex
