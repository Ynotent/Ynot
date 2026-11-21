// A small demo project used by tests and the CLI. Includes portable builtin
// tracks plus desktop-only plugins, so cross-device behaviour can be exercised.
#pragma once

#include <string>

#include "ynot/model.hpp"

namespace ynot {

// Creates the project and writes its media into `packageDir`.
Project makeDemoProject(const std::string& packageDir);

PluginInstance makeBuiltin(const std::string& uid, const std::string& name);
PluginInstance makeExternal(PluginFormat f, const std::string& uid, const std::string& name,
                            const std::string& vendor);

}  // namespace ynot
