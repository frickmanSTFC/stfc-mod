#pragma once

// Append-only JSONL milestone log: one line every time something notable changes level or tier
// (buildings, research, ships). Same shape and the same idea as the loot log — the server only
// ever sends current state, so the viewer diffs consecutive values to work out what happened when.
//
// The first value seen for a thing in a session has no "from" and is a baseline, not an upgrade.
// Baselines are still written so that a change made while the game was closed shows up as the
// jump between one session's last value and the next session's first.

#include "file.h"
#include "patches/parts/spec_ids.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace milestones {

inline std::mutex file_mtx;

inline void record(const char* kind, int64_t id, int64_t value, const std::string& name = "")
{
  const std::string path(File::ExportPath("community_patch_milestones.jsonl"));
  const auto        ts = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

  // Only these two have a spec the game can name; ship ids are per-ship instances.
  if (std::string_view(kind) == "building" || std::string_view(kind) == "research") {
    spec_ids::add(kind, id);
  }

  nlohmann::json j{{"t", ts}, {"kind", kind}, {"id", id}, {"v", value}};
  if (!name.empty()) {
    j["name"] = name;
  }

  std::scoped_lock lk(file_mtx);
  std::ofstream    f(path, std::ios::app);
  if (f) {
    f << j.dump() << "\n";
  }
}

// Same log, but the value is a JSON blob rather than a level — used for a ship's fitted component
// list, which the viewer diffs from one reading to the next to spot a module upgrade.
inline void record_json(const char* kind, int64_t id, const nlohmann::json& value)
{
  const std::string path(File::ExportPath("community_patch_milestones.jsonl"));
  const auto        ts = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

  const nlohmann::json j{{"t", ts}, {"kind", kind}, {"id", id}, {"v", value}};

  std::scoped_lock lk(file_mtx);
  std::ofstream    f(path, std::ios::app);
  if (f) {
    f << j.dump() << "\n";
  }
}

}  // namespace milestones
