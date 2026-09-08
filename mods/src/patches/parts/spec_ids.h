#pragma once

// Ids seen by the loot and milestone logs that still need a display name.
// sync.cc pushes them in; fleet_export.cc drains them and resolves each one through the game's
// own spec service, because the logs only ever record numeric ids.
// ponytail: a set behind a mutex. A few thousand ids in the first minute, then a trickle.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spec_ids {

using entry = std::pair<std::string, int64_t>;   // {kind, id}

inline std::mutex     mtx;
inline std::set<entry> pending;

inline void add(std::string kind, int64_t id)
{
  std::scoped_lock lk(mtx);
  pending.emplace(std::move(kind), id);
}

inline std::vector<entry> drain()
{
  std::scoped_lock   lk(mtx);
  std::vector<entry> out(pending.begin(), pending.end());
  pending.clear();
  return out;
}

}  // namespace spec_ids
