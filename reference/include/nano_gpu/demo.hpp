#pragma once

#include "nano_gpu/reference.hpp"

namespace nano_gpu {

enum class DemoScene { Showcase, Perspective, Overdraw };

// CPU scene preparation only: every generated draw uses the M0 render profile.
Pass make_demo_pass(DemoScene scene, int target, long double time_seconds);
const char* scene_name(DemoScene scene);

} // namespace nano_gpu
