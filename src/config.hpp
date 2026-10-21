// config.hpp - "key = value" settings file.
#pragma once
#include <string>

#include "params.hpp"

namespace imp {

// Unknown keys are reported in *warnings but do not fail the load.
bool load_config(const std::string& path, Params& p, StartupSettings& s, std::string* warnings);
bool save_config(const std::string& path, const Params& p, const StartupSettings& s);

}  // namespace imp
