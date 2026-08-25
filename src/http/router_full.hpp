#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
namespace examvan {
void register_full_routes(Router& r, const Config& cfg);
} // namespace examvan
