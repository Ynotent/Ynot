// The universal Ynot project format.
//
// A project is a package directory (shown as a single document on macOS/iOS):
//
//   MySong.ynot/
//     project.json        <- everything except audio, UTF-8 JSON, schema below
//     media/<sha256>.wav  <- immutable, content-addressed recordings/renders
//
// The same bytes are produced on every OS. Unknown fields written by a newer
// app version are preserved (Track::extras / Project::extras), so opening a
// project on an older device never silently destroys data. See
// docs/architecture/PROJECT_FORMAT.md.
#pragma once

#include <string>

#include "ynot/json.hpp"
#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

constexpr int kCurrentFormatVersion = 1;
constexpr const char* kFormatMagic = "ynot.project";

Json projectToJson(const Project& p);
Project projectFromJson(const Json& j);  // migrates older versions; throws on newer-major
Json trackToJson(const Track& t);
Track trackFromJson(const Json& j);

// Stable fingerprint of what a track *sounds* like (clips, instrument, inserts,
// automation) — excludes mix-only fields and the freeze itself. Used to detect
// stale freezes after the source track is edited.
std::string trackFingerprint(const Track& t);

// Package I/O.
void saveProjectPackage(const Project& p, const std::string& dir);
Project loadProjectPackage(const std::string& dir);

// Adds audio to the package's media store (content-addressed) and registers it.
const MediaRef& addMedia(Project& p, const std::string& packageDir, const AudioBuffer& audio,
                         const std::string& origin);
AudioBuffer loadMedia(const MediaRef& m, const std::string& packageDir);

}  // namespace ynot
