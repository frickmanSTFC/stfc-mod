// Fleet export: every few seconds, write a JSON snapshot of the player's fleets
// (state, location, mining progress, cargo) to community_patch_fleets.json in the
// game folder. A tiny external viewer (C:\DEV\STFC\viewer) reads that file.

#include "config.h"
#include "errormsg.h"
#include "file.h"
#include "str_utils.h"

#include <il2cpp/il2cpp_helper.h>
#include <prime/FleetPlayerData.h>
#include <prime/FleetsManager.h>
#include <prime/HullSpec.h>
#include <prime/ScreenManager.h>

#include "patches/parts/spec_ids.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spud/detour.h>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define FILE_DEF_FLEETS "community_patch_fleets.json"
#define FILE_DEF_RESOURCES "community_patch_resources.json"
#define FILE_DEF_SPECS "community_patch_specs.json"
#define FILE_DEF_ICONS "community_patch_icons.json"
#define FILE_DEF_SCAN "community_patch_scan_icons"

// ponytail: reflection-by-name property getters. No per-class header needed.
// Slow-ish (name lookup per call) but we run once per ~3s; cache MethodInfo if it ever matters.
static Il2CppObject* prop_obj(Il2CppObject* obj, const char* name)
{
  if (!obj) {
    return nullptr;
  }
  auto klass = il2cpp_object_get_class(obj);
  auto p     = il2cpp_class_get_property_from_name(klass, name);
  if (!p) {
    return nullptr;
  }
  auto getter = il2cpp_property_get_get_method((PropertyInfo*)p);
  if (!getter) {
    return nullptr;
  }
  Il2CppException* ex = nullptr;
  auto             r  = il2cpp_runtime_invoke(getter, obj, nullptr, &ex);
  return ex ? nullptr : r;
}

template <typename T> static T prop_val(Il2CppObject* obj, const char* name, T def = T{})
{
  auto r = prop_obj(obj, name);
  return r ? *(T*)il2cpp_object_unbox(r) : def;
}

// Resource id -> display name via SpecService.GetResourceSpec(id).Name. SpecService instance is
// borrowed from FleetsManager._specServiceCache (CachedService<T> is a one-pointer struct).
struct ResourceInfo {
  std::string name;     // internal spec name, e.g. Resource_G6_Crystal_Raw
  int64_t     loca_id;  // key into the game's "materials" translation table
  int64_t     art_id;   // IdRefs.ArtId
  std::string art_ref;  // IdRefs.ArtFileReference (sprite name in resource_icons bundle)
  int32_t     grade;    // the 1-6 star grade
  int32_t     rarity;
  int32_t     subtype;  // the game's own category: RawMaterial, Token, FactionPoint, ...
  int32_t     sort;     // SortingIndex, the order the game itself lists them in
};

static IL2CppClassHelper& spec_service_helper()
{
  static auto h = il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Services", "SpecService");
  return h;
}

// SpecService instance is borrowed from FleetsManager._specServiceCache (CachedService<T> is a
// one-pointer struct that fills lazily; call its get_Service so it resolves, then read the pointer).
static void* spec_service_for(FleetsManager* manager)
{
  static auto fm_helper   = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Prime.FleetManagement", "FleetsManager");
  static auto cache_field = fm_helper.GetField("_specServiceCache");
  if (!manager || !cache_field.isValidHelper()) {
    return nullptr;
  }

  auto        cache_ptr   = (char*)manager + cache_field.offset();
  static auto get_service = [&]() -> const MethodInfo* {
    auto fi = il2cpp_class_get_field_from_name(fm_helper.get_cls(), "_specServiceCache");
    if (!fi) return nullptr;
    auto cls = il2cpp_class_from_type(il2cpp_field_get_type(fi));
    return cls ? il2cpp_class_get_method_from_name(cls, "get_Service", 0) : nullptr;
  }();

  void* spec_service = nullptr;
  if (get_service) {
    Il2CppException* ex = nullptr;
    spec_service         = il2cpp_runtime_invoke(get_service, cache_ptr, nullptr, &ex);
    if (ex) spec_service = nullptr;
  }
  return spec_service ? spec_service : *(void**)cache_ptr;
}

static ResourceInfo resource_info(FleetsManager* manager, int64_t resource_id)
{
  static std::unordered_map<int64_t, ResourceInfo> cache;
  if (auto it = cache.find(resource_id); it != cache.end()) {
    return it->second;
  }

  static auto get_spec = spec_service_helper().GetMethod<Il2CppObject*(void*, int64_t)>("GetResourceSpec");

  ResourceInfo info{};
  if (get_spec) {
    void* spec_service = spec_service_for(manager);
    if (spec_service) {
      if (auto spec = get_spec(spec_service, resource_id)) {
        if (auto s = (Il2CppString*)prop_obj(spec, "Name")) {
          info.name = to_string(s);
        }
        info.grade   = prop_val<int32_t>(spec, "Grade");
        info.rarity  = prop_val<int32_t>(spec, "Rarity");
        info.subtype = prop_val<int32_t>(spec, "Subtype");
        info.sort    = prop_val<int32_t>(spec, "SortingIndex");
        if (auto refs = prop_obj(spec, "IdRefs")) {
          info.loca_id = prop_val<int64_t>(refs, "LocaId");
          info.art_id  = prop_val<int64_t>(refs, "ArtId");
          if (auto ar = (Il2CppString*)prop_obj(refs, "ArtFileReference")) {
            info.art_ref = to_string(ar);
          }
        }
      }
    }
  }
  if (!info.name.empty()) {
    cache[resource_id] = info;  // only cache hits; service may not be ready early on
  }
  return info;
}

// --- raw field / collection readers ------------------------------------------------------------
static void* field_ptr(Il2CppObject* obj, const char* name)
{
  if (!obj) {
    return nullptr;
  }
  auto f = il2cpp_class_get_field_from_name(il2cpp_object_get_class(obj), name);
  return f ? (char*)obj + il2cpp_field_get_offset(f) : nullptr;
}

// List<T> of reference types: walk _items[0.._size)
template <typename F> static void for_each_list(Il2CppObject* list, F fn)
{
  auto items_p = (Il2CppArray**)field_ptr(list, "_items");
  auto size_p  = (int32_t*)field_ptr(list, "_size");
  if (!items_p) {  // Google.Protobuf RepeatedField<T> keeps the same shape under other names
    items_p = (Il2CppArray**)field_ptr(list, "array");
    size_p  = (int32_t*)field_ptr(list, "count");
  }
  if (!items_p || !*items_p || !size_p) {
    return;
  }
  auto items = *items_p;
  auto n     = std::min<int32_t>(*size_p, (int32_t)il2cpp_array_length(items));
  for (int32_t i = 0; i < n; ++i) {
    if (auto e = *(Il2CppObject**)il2cpp_array_addr_with_size(items, i, sizeof(void*))) {
      fn(e);
    }
  }
}

// Dictionary<long,long>: walk _entries[0.._count); entry = {int hashCode; int next; long key; long value}
template <typename F> static void for_each_dict_ll(Il2CppObject* dict, F fn)
{
  auto entries_p = (Il2CppArray**)field_ptr(dict, "_entries");
  auto count_p   = (int32_t*)field_ptr(dict, "_count");
  if (!entries_p || !*entries_p || !count_p) {
    return;
  }
  auto entries = *entries_p;
  auto stride  = il2cpp_array_element_size(il2cpp_object_get_class((Il2CppObject*)entries));
  if (stride < 24) {
    return;
  }
  auto n = std::min<int32_t>(*count_p, (int32_t)il2cpp_array_length(entries));
  for (int32_t i = 0; i < n; ++i) {
    auto e    = (char*)il2cpp_array_addr_with_size(entries, i, stride);
    auto next = *(int32_t*)(e + 4);
    auto key  = *(int64_t*)(e + 8);
    if (next < -1 || key == 0) {  // freed slot
      continue;
    }
    fn(key, *(int64_t*)(e + 16));
  }
}

// FleetPlayerData.GetStat(StatType).CurrentValue; StatType is a 4-byte struct read from its static field.
static double stat_value(Il2CppObject* fleet, const char* stat_name)
{
  static auto stat_cls = [] {
    auto h = il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.Prime.Stats", "StatType");
    return h.isValidHelper() ? h : il2cpp_get_class_helper("Assembly-CSharp", "Digit.Prime.Stats", "StatType");
  }();
  static auto fleet_helper =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Models", "FleetPlayerData");
  static auto get_stat = fleet_helper.GetMethod<Il2CppObject*(void*, int32_t)>("GetStat", 1);
  if (!stat_cls.isValidHelper() || !get_stat) {
    return 0;
  }
  auto f = il2cpp_class_get_field_from_name(stat_cls.get_cls(), stat_name);
  if (!f) {
    return 0;
  }
  int32_t v = 0;
  il2cpp_field_static_get_value(f, &v);
  auto pd = get_stat(fleet, v);
  return pd ? prop_val<double>(pd, "CurrentValue") : 0;
}

static double ticks_to_s(int64_t ticks)
{
  return ticks / 10000000.0;
}

static std::string localize(const std::string& category, const std::string& identifier);
static void        note_hull(int64_t hull_id, const std::string& name);
static std::string localize_one(const std::string& category, const std::string& identifier);
static void        note_officer(int64_t spec_id, const std::string& name);
static void write_image_map();
static void write_avatar_names();

static const char* state_label(int s)
{
  // FleetState is a bit mask; report the most interesting bit.
  if (s & 8) return "destroyed";
  if (s & 64) return "battling";
  if (s & 4) return "mining";
  if (s & 2048) return "auto-hunting";
  if (s & 1024) return "capturing";
  if (s & 4096) return "outposting";
  if (s & 256) return "warping";
  if (s & 128) return "warp-charging";
  if (s & 512) return "impulsing";
  if (s & 32) return "repairing";
  if (s & 16) return "tiering-up";
  if (s & 2) return "docked";
  if (s & 1) return "idle-in-space";
  return "unknown";
}

static nlohmann::json snapshot_fleet(FleetsManager* manager, int idx, FleetPlayerData* fleet)
{
  auto obj = (Il2CppObject*)fleet;
  nlohmann::json j;
  j["index"]     = idx;
  j["id"]        = (int64_t)fleet->Id;
  j["ship"]      = fleet->Hull ? to_string(fleet->Hull->Name) : "";
  if (fleet->Hull) {
    note_hull(fleet->Hull->Id, j["ship"].get<std::string>());
  }
  j["tier"]      = prop_val<int32_t>(obj, "Tier");
  auto state     = (int)fleet->CurrentState;
  j["state_raw"] = state;
  j["state"]     = state_label(state);

  if (auto addr = prop_obj(obj, "Address")) {
    j["system"] = prop_val<int64_t>(addr, "System");
    j["planet"] = prop_val<int64_t>(addr, "Planet");
    // NodeID is the deepest id in the address, so it names the mining node itself rather than the
    // planet it orbits. Instance separates two nodes that share one address.
    j["node_id"]  = prop_val<int64_t>(addr, "NodeID");
    j["instance"] = prop_val<int32_t>(addr, "Instance");
  }

  if (auto slot = prop_obj(obj, "MiningData")) {
    auto        ticks   = prop_val<int64_t>(slot, "RemainingTime");
    double      mined   = prop_val<double>(slot, "AmountMined");
    int64_t     total = 0, node_rate = 0;
    int32_t     level = 0;
    if (auto point = prop_obj(slot, "PointData")) {
      total     = prop_val<int64_t>(point, "Amount");
      node_rate = prop_val<int64_t>(point, "Rate");
      level     = prop_val<int32_t>(point, "Level");
    }
    auto resource_id = prop_val<int64_t>(slot, "ResourceId");
    auto resource    = resource_info(manager, resource_id);
    j["mining"] = {
        {"active", prop_val<bool>(slot, "IsActive")},
        {"resource_id", resource_id},
        {"resource", resource.name},
        {"resource_loca", resource.loca_id},
        {"resource_pretty", resource.loca_id ? localize("materials", std::to_string(resource.loca_id)) : ""},
        {"art_id", resource.art_id},
        {"art_ref", resource.art_ref},
        {"per_hour", prop_val<int64_t>(slot, "PerHourRate")},
        {"mined", mined},
        {"node_total", total},
        {"node_rate", node_rate},
        {"node_level", level},
        {"left", (double)total - mined},
        {"remaining_s", ticks / 10000000.0},
    };
  }

  if (auto hold = prop_obj(obj, "CargoHoldData")) {
    auto cur  = prop_obj(hold, "CurrentCargo");
    auto prot = prop_obj(hold, "ProtectedCargoProgress");
    j["cargo"] = {
        {"current", prop_val<double>(cur, "CurrentValue")},
        {"max", prop_val<double>(cur, "MaxValue")},
        {"protected", prop_val<double>(prot, "CurrentValue")},
        {"protected_max", prop_val<double>(prot, "MaxValue")},
        {"fill", prop_val<float>(obj, "CargoResourceFillLevel")},
    };
  }

  // cargo contents: Cargo.Resources is Dictionary<resourceId, amount>
  j["cargo_items"] = nlohmann::json::array();
  if (auto cargo = prop_obj(obj, "Cargo")) {
    for_each_dict_ll(prop_obj(cargo, "Resources"), [&](int64_t rid, int64_t amount) {
      if (amount > 0) {
        auto ri = resource_info(manager, rid);
        j["cargo_items"].push_back({{"id", rid},
                                    {"name", ri.name},
                                    {"pretty", ri.loca_id ? localize("materials", std::to_string(ri.loca_id)) : ""},
                                    {"art_id", ri.art_id},
                                    {"art_ref", ri.art_ref},
                                    {"amount", amount}});
      }
    });
  }

  // ship condition
  if (auto ship = prop_obj(obj, "Ship")) {
    j["level"]         = prop_val<int32_t>(ship, "Level");
    j["level_pct"]     = prop_val<float>(ship, "LevelPercentage");
    j["hull_damage"]   = prop_val<float>(ship, "HullDamage");
    j["shield_damage"] = prop_val<float>(ship, "ShieldDamage");
  }
  j["damaged"]   = prop_val<bool>(obj, "IsDamaged");
  j["hull_type"] = fleet->Hull ? (int)fleet->Hull->Type : -1;
  j["stats"]     = {
      {"hull_hp", stat_value(obj, "HullHP")},           {"hull_hp_now", stat_value(obj, "CurrentHullHP")},
      {"shield_hp", stat_value(obj, "ShieldHP")},       {"shield_hp_now", stat_value(obj, "CurrentShieldHP")},
      {"strength", stat_value(obj, "ShipStrength")},    {"attack", stat_value(obj, "Attack")},
      {"defense", stat_value(obj, "Defense")},          {"health", stat_value(obj, "Health")},
      {"warp_range", stat_value(obj, "WarpDistance")},  {"warp_speed", stat_value(obj, "WarpSpeed")},
      {"impulse", stat_value(obj, "ImpulseVelocity")},
  };

  // officers (bridge first, then below deck)
  j["officers"] = nlohmann::json::array();
  for_each_list(prop_obj(obj, "FleetOfficers"), [&](Il2CppObject* o) {
    auto        spec    = prop_obj(o, "Spec");
    int64_t     loca_id = 0, art_id = 0;
    std::string art_ref;
    if (auto refs = prop_obj(spec, "IdRefs")) {
      loca_id = prop_val<int64_t>(refs, "LocaId");
      art_id  = prop_val<int64_t>(refs, "ArtId");
      if (auto ar = (Il2CppString*)prop_obj(refs, "ArtFileReference")) {
        art_ref = to_string(ar);
      }
    }
    std::string name;
    if (loca_id) {
      name = localize("officer_names", std::to_string(loca_id));
    }
    // This path works, so bank the name for the battle log too: SpecService.GetOfficerSpec only
    // answers for some ids, but every officer that crews a ship passes through here.
    note_officer(prop_val<int64_t>(spec, "Id"), name);
    j["officers"].push_back({{"id", prop_val<int64_t>(o, "Id")},
                             {"spec_id", prop_val<int64_t>(spec, "Id")},
                             {"loca_id", loca_id},
                             {"art_id", art_id},
                             {"art_ref", art_ref},
                             {"name", name},
                             {"level", prop_val<int32_t>(o, "Level")},
                             {"rank", prop_val<int32_t>(o, "CurrentRank")}});
  });

  // timers: repair / tier-up / warp etc. surface through Timer; active course has its own
  if (auto timer = prop_obj(obj, "Timer")) {
    j["timer_s"] = ticks_to_s(prop_val<int64_t>(timer, "RemainingTime"));
  }
  if (auto course = prop_obj(obj, "ActiveCourse")) {
    j["course_s"] = ticks_to_s(prop_val<int64_t>(course, "RemainingTime"));
  }
  return j;
}

#define FILE_DEF_MINING "community_patch_mining.jsonl"

// One line per finished mining session.
//
// The game only ever says "this ship is mining that node right now", so a session has to be closed
// out by watching for the moment it changes: a different node, a different resource on the same
// node, or the ship no longer mining at all. Nothing is inferred about what the node *should* pay
// out -- the line records what the node said it held and how much came out of it.
struct MiningSession {
  int64_t node_id = 0, resource_id = 0, node_total = 0, node_rate = 0;
  int32_t instance = 0, level = 0;
  int64_t system = 0, planet = 0;
  int64_t t_start = 0;
  double  mined = 0;
  double  left = 0;
};

static void write_mining_line(const MiningSession& s, const char* ended, int64_t now)
{
  const std::string path(File::ExportPath(FILE_DEF_MINING));
  std::ofstream     f(path, std::ios::app);
  if (!f) {
    spdlog::warn("Mining log: cannot write {}", path);
    return;
  }
  nlohmann::json j = {
      {"t0", s.t_start},      {"t", now},          {"node", s.node_id},  {"inst", s.instance},
      {"sys", s.system},      {"planet", s.planet}, {"res", s.resource_id}, {"level", s.level},
      {"node_total", s.node_total}, {"node_rate", s.node_rate},
      {"mined", s.mined},     {"left", s.left},    {"end", ended},
  };
  f << j.dump() << "\n";
}

// Called once per fleet per tick with that fleet's snapshot.
static void record_mining(int idx, const nlohmann::json& j)
{
  static std::map<int, MiningSession> live;

  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  const bool mining = j.value("state", "") == "mining" && j.contains("mining");
  auto       it     = live.find(idx);

  if (!mining) {
    if (it != live.end()) {
      write_mining_line(it->second, "stopped", now);
      live.erase(it);
    }
    return;
  }

  const auto& m = j["mining"];
  MiningSession cur;
  cur.node_id     = j.value("node_id", (int64_t)0);
  cur.instance    = j.value("instance", 0);
  cur.system      = j.value("system", (int64_t)0);
  cur.planet      = j.value("planet", (int64_t)0);
  cur.resource_id = m.value("resource_id", (int64_t)0);
  cur.node_total  = m.value("node_total", (int64_t)0);
  cur.node_rate   = m.value("node_rate", (int64_t)0);
  cur.level       = m.value("node_level", 0);
  cur.mined       = m.value("mined", 0.0);
  cur.left        = m.value("left", 0.0);

  if (it != live.end()) {
    const auto& p = it->second;
    // A node keeping its id but swapping the resource under you is exactly the case worth catching,
    // so the resource is part of what makes a session, not just the node.
    if (p.node_id == cur.node_id && p.instance == cur.instance && p.resource_id == cur.resource_id) {
      it->second.mined      = cur.mined;      // same session, just further along
      it->second.left       = cur.left;
      it->second.node_total = cur.node_total;
      return;
    }
    write_mining_line(p, p.node_id == cur.node_id && p.instance == cur.instance ? "changed" : "moved",
                      now);
    live.erase(it);
  }

  cur.t_start = now;
  live[idx]   = cur;
  // Also write the moment the session opens. A session can run for hours, so waiting for it to end
  // would lose the whole thing if the game is closed mid-mine.
  write_mining_line(cur, "start", now);
}

static void export_fleets()
{
  auto manager = FleetsManager::Instance();
  if (!manager) {
    return;
  }

  nlohmann::json out;
  out["updated"] = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  out["fleets"] = nlohmann::json::array();

  // ponytail: fixed index scan; game exposes no fleet count. Same bound hotkeys.cc uses; raise if drydocks ever exceed 10.
  for (int idx = 0; idx < 10; ++idx) {
    auto fleet = manager->GetFleetPlayerData(idx);
    if (!fleet || !prop_val<bool>((Il2CppObject*)fleet, "HasShip")) {
      continue;
    }
    auto snap = snapshot_fleet(manager, idx, fleet);
    record_mining(idx, snap);
    out["fleets"].push_back(std::move(snap));
  }

  const std::string path = std::string(File::ExportPath(FILE_DEF_FLEETS));
  const std::string tmp  = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << out.dump(2);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
}

// --- name dictionaries --------------------------------------------------------------------------
// The loot and milestone logs only record numeric ids. Resolve each new id through the game's own
// spec service and keep two JSON files topped up for the viewer:
//   community_patch_resources.json  resources  (id -> name, pretty, icon)
//   community_patch_specs.json      buildings, research and ship hulls
// Ids that cannot be resolved yet go back on the queue and are retried on the next tick.

static nlohmann::json g_resources   = nlohmann::json::object();
static bool           g_specs_dirty = false;
static nlohmann::json g_specs     = {{"building", nlohmann::json::object()},
                                     {"research", nlohmann::json::object()},
                                     {"hull", nlohmann::json::object()},
                                     {"hull_name", nlohmann::json::object()},
                                     {"component_name", nlohmann::json::object()},
                                     {"officer", nlohmann::json::object()},
                                     {"faction", nlohmann::json::object()}};

static void write_json_file(const char* filename, const nlohmann::json& body)
{
  const std::string path = std::string(File::ExportPath(filename));
  const std::string tmp  = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << body.dump(2);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
}

// SpecService.GetStarbaseSpec(id) -> StarbaseSpec{Name, Type}. Buildings carry no LocaId, so the
// display name is tried against the starbase_modules table and falls back to the internal name.
static bool resolve_building(void* spec_service, int64_t id, nlohmann::json& out)
{
  static auto get_spec = spec_service_helper().GetMethod<Il2CppObject*(void*, int64_t)>("GetStarbaseSpec");
  if (!get_spec) {
    return false;
  }
  auto spec = get_spec(spec_service, id);
  if (!spec) {
    return false;
  }
  std::string name;
  if (auto s = (Il2CppString*)prop_obj(spec, "Name")) {
    name = to_string(s);
  }
  if (name.empty()) {
    return false;
  }
  out = {{"name", name}, {"pretty", localize("starbase_modules", std::to_string(id))}};
  if (auto t = (Il2CppString*)prop_obj(spec, "Type")) {
    out["type"] = to_string(t);
  }
  return true;
}

// SpecService.TryGetResearchProjectSpec(id, out spec) -> IdRefs.LocaId -> the research text table.
static bool resolve_research(void* spec_service, int64_t id, nlohmann::json& out)
{
  static auto try_get =
      spec_service_helper().GetMethod<bool(void*, int64_t, Il2CppObject**)>("TryGetResearchProjectSpec");
  if (!try_get) {
    static bool moaned = false;
    if (!moaned) {
      moaned = true;
      spdlog::warn("Fleet export: SpecService.TryGetResearchProjectSpec not found; research stays unnamed");
    }
    return false;
  }
  Il2CppObject* spec = nullptr;
  if (!try_get(spec_service, id, &spec) || !spec) {
    return false;
  }
  auto refs = prop_obj(spec, "IdRefs");
  if (!refs) {
    return false;
  }
  // The loca id is enough on its own: the viewer can look it up in the research text table even
  // when the in-game localiser declines to answer, so do not fail the whole entry over a name.
  const auto loca = prop_val<int64_t>(refs, "LocaId");
  out             = {{"pretty", loca ? localize("research", std::to_string(loca)) : std::string()},
                     {"loca_id", loca},
                     {"tree", prop_val<int64_t>(spec, "ResearchTreeId")}};
  return true;
}

// --- art id -> icon name --------------------------------------------------------------------------
// The game keys every item icon by an identifier like "resource/icon_9709". That identifier is
// hashed and looked up in DynamicSpriteDatabase, so ask the game for the hash and read the answer
// back. Nothing here is guessed: a resource either gets its own icon or none.

// Dictionary<int, T> entry layout is {uint hash; int next; int key; TValue value}, so with the
// value aligned to 8 the key sits at +8 and the value at +16.
template <typename F> static void for_each_dict_int(Il2CppObject* dict, F fn)
{
  auto entries_p = (Il2CppArray**)field_ptr(dict, "_entries");
  auto count_p   = (int32_t*)field_ptr(dict, "_count");
  if (!entries_p || !*entries_p || !count_p) {
    return;
  }
  auto entries = *entries_p;
  auto stride  = il2cpp_array_element_size(il2cpp_object_get_class((Il2CppObject*)entries));
  if (stride < 24) {
    return;
  }
  auto n = std::min<int32_t>(*count_p, (int32_t)il2cpp_array_length(entries));
  for (int32_t i = 0; i < n; ++i) {
    auto e    = (char*)il2cpp_array_addr_with_size(entries, i, stride);
    auto next = *(int32_t*)(e + 4);
    if (next < -1) {  // freed slot
      continue;
    }
    fn(*(int32_t*)(e + 8), e + 16);
  }
}

// StringExtensions.GetLegacyHashCode — the other candidate for how the sprite database keys itself.
static int32_t legacy_hash(const std::string& text)
{
  static auto helper = il2cpp_get_class_helper("Assembly-CSharp", "", "StringExtensions");
  static auto method =
      helper.isValidHelper() ? il2cpp_class_get_method_from_name(helper.get_cls(), "GetLegacyHashCode", 1) : nullptr;
  if (!method) {
    return 0;
  }
  void*            args[1] = {il2cpp_string_new(text.c_str())};
  Il2CppException* ex      = nullptr;
  auto             r       = il2cpp_runtime_invoke(method, nullptr, args, &ex);
  return (ex || !r) ? 0 : *(int32_t*)il2cpp_object_unbox(r);
}

static int32_t identifier_hash(const std::string& identifier)
{
  static auto ctx_helper = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.UI", "LocaleTextContext");
  static auto method     = ctx_helper.isValidHelper()
                               ? il2cpp_class_get_method_from_name(ctx_helper.get_cls(), "CalculateIdentifierHash", 2)
                               : nullptr;
  if (!method) {
    return 0;
  }
  void*            args[2] = {il2cpp_string_new(identifier.c_str()), nullptr};
  Il2CppException* ex      = nullptr;
  auto             r       = il2cpp_runtime_invoke(method, nullptr, args, &ex);
  return (ex || !r) ? 0 : *(int32_t*)il2cpp_object_unbox(r);
}

// The sprite databases key themselves by a hash we cannot reproduce, but each entry carries its own
// identifier as text ("resource/icon_7995") and an AssetBundleResource that names the sprite once the
// game has loaded it. So read both directly: no hashing, no guessing.
//
// Only icons the game has actually drawn carry a loaded asset name, so this fills in as you visit
// screens. It is written every time it grows.
static void write_icon_map()
{
  // Carried over from previous sessions: the game only names an icon once it has drawn it, so the
  // map is built up over time. Starting empty each launch would throw that away.
  static nlohmann::json known = [] {
    nlohmann::json loaded = nlohmann::json::object();
    std::ifstream  f(std::string(File::ExportPath(FILE_DEF_ICONS)));
    if (f) {
      try {
        auto j = nlohmann::json::parse(f);
        if (j.is_object()) {
          // Entries from before the key changed are bare art ids ("51"). Those were the ones that
          // overwrote each other, so they are dropped rather than trusted.
          for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key().find('/') != std::string::npos) {
              loaded[it.key()] = it.value();
            }
          }
        }
      } catch (const std::exception&) {
        // a half-written file is no reason to lose the session
      }
    }
    return loaded;
  }();
  static std::unordered_set<int32_t> answered;      // entries already resolved, never re-read

  // Only ever runs when asked. The walk covers millions of entries and sits on the game thread, so
  // it must not happen on a timer — the viewer drops a marker file when you press "update icons".
  const std::string trigger(File::ExportPath(FILE_DEF_SCAN));
  std::error_code   ec;
  if (!std::filesystem::exists(trigger, ec)) {
    return;
  }
  std::filesystem::remove(trigger, ec);

  static auto dsd = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.UI", "DynamicSpriteDatabase");
  if (!dsd.isValidHelper()) {
    return;
  }
  auto field = il2cpp_class_get_field_from_name(dsd.get_cls(), "_staticDB");
  if (!field) {
    return;
  }
  Il2CppObject* static_db = nullptr;
  il2cpp_field_static_get_value(field, &static_db);
  if (!static_db) {
    return;
  }

  static auto object_name = [] {
    auto h = il2cpp_get_class_helper("UnityEngine.CoreModule", "UnityEngine", "Object");
    return h.isValidHelper() ? h.GetMethod<Il2CppString*(void*)>("get_name") : nullptr;
  }();

  int added = 0, seen = 0;
  for_each_dict_int(static_db, [&](int32_t, char* value) {
    auto db = *(Il2CppObject**)value;
    if (!db) {
      return;
    }
    auto by_hash = (Il2CppObject**)field_ptr(db, "_resourcesByHashedID");
    if (!by_hash || !*by_hash) {
      return;
    }
    // NamedResource is {string Name; AssetBundleResource Resource}.
    // Order matters for speed: the same entries repeat across every database, so millions get
    // looked at per pass. Do only pointer reads until an entry is known to be worth the string work.
    for_each_dict_int(*by_hash, [&](int32_t key, char* named) {
      ++seen;
      if (!answered.insert(key).second) {
        return;                                     // already named from an earlier pass
      }
      auto resource = *(Il2CppObject**)(named + 8);
      auto name_str = *(Il2CppString**)named;
      if (!resource || !name_str) {
        answered.erase(key);
        return;
      }

      auto        loaded = (Il2CppString**)field_ptr(resource, "<LoadedAssetName>k__BackingField");
      std::string asset;
      if (loaded && *loaded) {
        asset = to_string(*loaded);
      } else if (object_name) {
        auto p = (Il2CppObject**)field_ptr(resource, "_loadedResource");
        if (p && *p) {
          if (auto n = object_name(*p)) {
            asset = to_string(n);
          }
        }
      }
      if (asset.empty()) {
        answered.erase(key);                        // not drawn yet; look again on a later pass
        return;
      }

      // The identifier is "<kind>/icon_<artId>", e.g. "resource/icon_51". The art id is only unique
      // inside its kind -- buff/icon_51 and resource/icon_51 are different pictures -- so the whole
      // identifier is the key. Keying on the number alone let one kind overwrite another.
      const auto identifier = to_string(name_str);
      if (identifier.empty() || known.contains(identifier)) {
        return;
      }
      known[identifier] = asset;
      ++added;
    });
  });

  write_json_file(FILE_DEF_ICONS, known);
  spdlog::info("Icon map: scan requested, {} entries checked, {} art ids named (+{})", seen, known.size(), added);
}

static void write_spec_names()
{
  auto ids = spec_ids::drain();
  if (ids.empty()) {
    return;
  }
  auto manager      = FleetsManager::Instance();
  auto spec_service = manager ? spec_service_for(manager) : nullptr;
  if (!spec_service) {
    for (auto& e : ids) {  // specs not reachable yet
      spec_ids::add(e.first, e.second);
    }
    return;
  }

  int resources_added = 0, specs_added = 0;
  for (auto& [kind, id] : ids) {
    const auto key = std::to_string(id);
    if (kind == "resource") {
      if (g_resources.contains(key)) {
        continue;
      }
      auto info = resource_info(manager, id);
      if (info.name.empty()) {
        spec_ids::add(kind, id);
        continue;
      }
      g_resources[key] = {{"name", info.name},
                          {"pretty", info.loca_id ? localize("materials", std::to_string(info.loca_id)) : ""},
                          {"art_id", info.art_id},
                          {"art_ref", info.art_ref},
                          {"grade", info.grade},
                          {"rarity", info.rarity},
                          {"subtype", info.subtype},
                          {"sort", info.sort}};
      ++resources_added;
      continue;
    }

    if (kind == "faction") {
      if (g_specs["faction"].contains(key)) {
        continue;
      }
      static auto try_faction =
          spec_service_helper().GetMethod<bool(void*, int64_t, Il2CppObject**)>("TryGetFactionsSpec");
      Il2CppObject* spec = nullptr;
      if (!try_faction || !try_faction(spec_service, id, &spec) || !spec) {
        spec_ids::add(kind, id);
        continue;
      }
      std::string name;
      if (auto s = (Il2CppString*)prop_obj(spec, "Name")) {
        name = to_string(s);
      }
      auto       refs   = prop_obj(spec, "IdRefs");
      const auto loca   = refs ? prop_val<int64_t>(refs, "LocaId") : 0;
      const auto pretty = loca ? localize("factions", std::to_string(loca)) : std::string();
      if (name.empty() && pretty.empty()) {
        spec_ids::add(kind, id);
        continue;
      }
      g_specs["faction"][key] = pretty.empty() ? name : pretty;
      ++specs_added;
      continue;
    }

    if (kind == "officer") {
      if (g_specs["officer"].contains(key)) {
        continue;
      }
      static auto get_officer = spec_service_helper().GetMethod<Il2CppObject*(void*, int64_t)>("GetOfficerSpec");
      auto        spec        = get_officer ? get_officer(spec_service, id) : nullptr;
      auto        refs        = spec ? prop_obj(spec, "IdRefs") : nullptr;
      const auto  loca        = refs ? prop_val<int64_t>(refs, "LocaId") : 0;
      const auto  pretty      = loca ? localize("officer_names", std::to_string(loca)) : std::string();
      if (pretty.empty()) {
        static int moans = 0;
        if (moans < 5) {
          ++moans;
          spdlog::info("Fleet export: officer {} unnamed (spec {}, loca {})", id,
                       spec ? "found" : "missing", loca);
        }
        spec_ids::add(kind, id);
        continue;
      }
      g_specs["officer"][key] = pretty;
      ++specs_added;
      continue;
    }

    if (kind == "component_loca") {
      if (g_specs["component_name"].contains(key)) {
        continue;
      }
      const auto pretty = localize("ship_components", key);
      if (pretty.empty()) {
        spec_ids::add(kind, id);
        continue;
      }
      g_specs["component_name"][key] = pretty;
      ++specs_added;
      continue;
    }

    if (kind == "hull_loca") {
      if (g_specs["hull_name"].contains(key)) {
        continue;
      }
      const auto pretty = localize("ships", key);
      if (pretty.empty()) {
        spec_ids::add(kind, id);
        continue;
      }
      g_specs["hull_name"][key] = pretty;
      ++specs_added;
      continue;
    }

    if (g_specs[kind].contains(key)) {
      continue;
    }
    nlohmann::json entry;
    const bool     ok = kind == "building"   ? resolve_building(spec_service, id, entry)
                        : kind == "research" ? resolve_research(spec_service, id, entry)
                                             : false;
    if (!ok) {
      spec_ids::add(kind, id);
      continue;
    }
    g_specs[kind][key] = entry;
    ++specs_added;
  }

  if (resources_added) {
    write_json_file(FILE_DEF_RESOURCES, g_resources);
    spdlog::info("Fleet export: {} has {} resources (+{})", FILE_DEF_RESOURCES, g_resources.size(), resources_added);
  }
  if (specs_added) {
    write_json_file(FILE_DEF_SPECS, g_specs);
    spdlog::info("Fleet export: {} has {} buildings, {} research (+{})", FILE_DEF_SPECS,
                 g_specs["building"].size(), g_specs["research"].size(), specs_added);
  }
}

// Ship hull names are only reachable through a fleet, so record each hull as it is seen flying.
static void note_officer(int64_t spec_id, const std::string& name)
{
  if (spec_id == 0 || name.empty()) {
    return;
  }
  const auto key = std::to_string(spec_id);
  if (g_specs["officer"].contains(key)) {
    return;
  }
  g_specs["officer"][key] = name;
  g_specs_dirty      = true;      // written once per tick, not once per name
}

static void note_hull(int64_t hull_id, const std::string& name)
{
  if (hull_id == 0 || name.empty()) {
    return;
  }
  const auto key = std::to_string(hull_id);
  if (g_specs["hull"].contains(key)) {
    return;
  }
  g_specs["hull"][key] = {{"name", name}};
  g_specs_dirty      = true;      // written once per tick, not once per name
}

// --- research catalogue -----------------------------------------------------------------------
// Every research project the game knows, with max level and what each level needs and costs.
// Written once per game start from SpecService.ResearchProjectSpecs / ResearchTreeSpecs; the
// viewer pairs it with the levels in the milestones log to show done / available / locked.
#define FILE_DEF_RESEARCH "community_patch_research.json"

// Google.Protobuf RepeatedField<T>: T[] array + int count
template <typename F> static void for_each_repeated(Il2CppObject* rf, F fn)
{
  auto array_p = (Il2CppArray**)field_ptr(rf, "array");
  auto count_p = (int32_t*)field_ptr(rf, "count");
  if (!array_p || !*array_p || !count_p) {
    return;
  }
  auto arr = *array_p;
  auto n   = std::min<int32_t>(*count_p, (int32_t)il2cpp_array_length(arr));
  for (int32_t i = 0; i < n; ++i) {
    fn(arr, i);
  }
}
static Il2CppObject* rf_obj(Il2CppArray* arr, int32_t i) { return *(Il2CppObject**)il2cpp_array_addr_with_size(arr, i, sizeof(void*)); }
static int64_t       rf_i64(Il2CppArray* arr, int32_t i) { return *(int64_t*)il2cpp_array_addr_with_size(arr, i, sizeof(int64_t)); }

// Google.Protobuf MapField<long, T>: entries live on a LinkedList<KeyValuePair<long, T>>;
// each node's inline item is {long key; T value}.
template <typename F> static void for_each_mapfield(Il2CppObject* map, F fn)
{
  auto list_p = (Il2CppObject**)field_ptr(map, "list");
  auto list   = list_p ? *list_p : nullptr;
  auto head_p = (Il2CppObject**)field_ptr(list, "head");
  auto count_p = (int32_t*)field_ptr(list, "count");
  if (!head_p || !*head_p || !count_p) {
    return;
  }
  auto node = *head_p;
  for (int32_t i = 0; i < *count_p && node; ++i) {
    auto item = (char*)field_ptr(node, "item");
    if (!item) {
      return;
    }
    fn(*(int64_t*)item, *(Il2CppObject**)(item + 8));
    auto next_p = (Il2CppObject**)field_ptr(node, "next");
    node        = next_p ? *next_p : nullptr;
  }
}

static void write_research_catalogue()
{
  static bool done = false;
  if (done) {
    return;
  }
  auto manager      = FleetsManager::Instance();
  auto spec_service = manager ? (Il2CppObject*)spec_service_for(manager) : nullptr;
  if (!spec_service) {
    return;
  }
  // the spec tables live on SpecService._dataContainer (StaticSyncDataContainer)
  auto container_p = (Il2CppObject**)field_ptr(spec_service, "_dataContainer");
  auto container   = container_p ? *container_p : nullptr;
  auto projects_p  = (Il2CppObject**)field_ptr(container, "ResearchProjectSpecs");
  auto trees_p     = (Il2CppObject**)field_ptr(container, "ResearchTreeSpecs");
  static int tries = 0;
  if (!projects_p || !*projects_p) {
    if (++tries % 20 == 1) {
      spdlog::warn("Research catalogue: ResearchProjectSpecs {} ({})", projects_p ? "is null" : "field not found",
                   container ? il2cpp_class_get_name(il2cpp_object_get_class(container)) : "no _dataContainer");
    }
    return;
  }
  if (tries == 0) {
    auto cls = il2cpp_object_get_class(*projects_p);
    spdlog::info("Research catalogue: dictionary type {} (namespace {})", il2cpp_class_get_name(cls),
                 il2cpp_class_get_namespace(cls));
    tries = 1;
  }

  nlohmann::json out;
  out["projects"] = nlohmann::json::object();
  out["trees"]    = nlohmann::json::object();
  int unnamed = 0;

  for_each_mapfield(*projects_p, [&](int64_t id, Il2CppObject* spec) {
    if (!spec) {
      return;
    }
    nlohmann::json p;
    p["tree"] = prop_val<int64_t>(spec, "ResearchTreeId");
    auto refs = prop_obj(spec, "IdRefs");
    auto loca = refs ? prop_val<int64_t>(refs, "LocaId") : 0;
    p["name"] = loca ? localize("research", std::to_string(loca)) : std::string();
    if (p["name"].get<std::string>().empty()) {
      ++unnamed;
      if (auto ls = refs ? (Il2CppString*)prop_obj(refs, "LocaStringId") : nullptr) {
        p["loca_str"] = to_string(ls);
        auto n        = localize("research", p["loca_str"].get<std::string>());
        if (!n.empty()) { p["name"] = n; --unnamed; }
      }
    }
    p["levels"] = nlohmann::json::array();
    for_each_repeated(prop_obj(spec, "Levels"), [&](Il2CppArray* arr, int32_t i) {
      auto lvl = rf_obj(arr, i);
      if (!lvl) {
        return;
      }
      nlohmann::json l;
      l["time"]  = prop_val<int64_t>(lvl, "ResearchTimeInSeconds");
      l["might"] = prop_val<int64_t>(lvl, "MilitaryMight");
      l["req"]   = nlohmann::json::array();
      for_each_repeated(prop_obj(lvl, "Requirements"), [&](Il2CppArray* ra, int32_t j) {
        auto r = rf_obj(ra, j);
        if (!r) {
          return;
        }
        auto type = prop_val<int32_t>(r, "Type");
        auto target = prop_val<int64_t>(r, "TargetId");
        l["req"].push_back({type, target, prop_val<int64_t>(r, "Level")});
        if (type == 1) spec_ids::add("building", target);
      });
      l["cost"] = nlohmann::json::array();
      for_each_repeated(prop_obj(lvl, "Costs"), [&](Il2CppArray* ca, int32_t j) {
        auto c = rf_obj(ca, j);
        if (!c) {
          return;
        }
        auto rid = prop_val<int64_t>(c, "ResourceId");
        l["cost"].push_back({rid, prop_val<int64_t>(c, "Value")});
        spec_ids::add("resource", rid);
      });
      p["levels"].push_back(std::move(l));
    });
    out["projects"][std::to_string(id)] = std::move(p);
  });

  if (trees_p && *trees_p) {
    for_each_list(*trees_p, [&](Il2CppObject* tree) {
      nlohmann::json t;
      auto id    = prop_val<int64_t>(tree, "Id");
      auto refs  = prop_obj(tree, "IdRefs");
      auto loca  = refs ? prop_val<int64_t>(refs, "LocaId") : 0;
      std::string loca_str;
      if (auto ls = refs ? (Il2CppString*)prop_obj(refs, "LocaStringId") : nullptr) {
        loca_str = to_string(ls);
      }
      // Tree names sit in the research table as research_tree_name_<LocaId> (found 2026-09-08).
      // A bare number is never tried there: it answers with some project's name.
      t["name"] = loca ? localize_one("research", "research_tree_name_" + std::to_string(loca)) : std::string();
      t["loca"]     = loca;
      t["loca_str"] = loca_str;
      t["type"]    = prop_val<int32_t>(tree, "Type");
      t["faction"] = prop_val<int64_t>(tree, "FactionId");
      t["projects"] = nlohmann::json::array();
      for_each_repeated(prop_obj(tree, "Projects"), [&](Il2CppArray* arr, int32_t i) {
        t["projects"].push_back(rf_i64(arr, i));
      });
      out["trees"][std::to_string(id)] = std::move(t);
    });
  }

  if (out["projects"].empty()) {
    if (tries++ % 20 == 1) {
      spdlog::warn("Research catalogue: MapField walk found nothing (list/head/count missing or empty)");
    }
    return;   // specs not loaded yet; try again next tick
  }
  write_json_file(FILE_DEF_RESEARCH, out);
  done = true;
  spdlog::info("Research catalogue: {} projects in {} trees written to {} ({} unnamed)", out["projects"].size(),
               out["trees"].size(), FILE_DEF_RESEARCH, unnamed);
}

static void ScreenManager_LateUpdate_Hook(auto original, ScreenManager* _this)
{
  original(_this);

  // ponytail: 3s fixed cadence; make it a config key if anyone asks.
  static auto next = std::chrono::steady_clock::now();
  auto        now  = std::chrono::steady_clock::now();
  if (now < next) {
    return;
  }
  next = now + std::chrono::seconds(3);
  export_fleets();
  write_research_catalogue();
  write_spec_names();
  write_icon_map();
  if (g_specs_dirty) {
    g_specs_dirty = false;
    write_json_file(FILE_DEF_SPECS, g_specs);
  }
  write_image_map();
  write_avatar_names();
}

// --- game localizer ---------------------------------------------------------------------------
// Ask the game itself: new LocaleTextContext(identifier, category) -> LanguageManager.Instance.Localize(out s, ctx)
// Build LocaleTextContext(identifier, category), UpdateHashes(), then LanguageManager.Localize(out s, ctx).
static std::string localize_one(const std::string& category, const std::string& identifier)
{
  static auto lm_helper  = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.Localization", "LanguageManager");
  static auto ctx_helper = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.UI", "LocaleTextContext");
  static const MethodInfo* ctx_ctor =
      ctx_helper.isValidHelper() ? il2cpp_class_get_method_from_name(ctx_helper.get_cls(), ".ctor", 2) : nullptr;
  static const MethodInfo* update_hashes =
      ctx_helper.isValidHelper() ? il2cpp_class_get_method_from_name(ctx_helper.get_cls(), "UpdateHashes", 0) : nullptr;
  static const MethodInfo* localize_m =
      lm_helper.isValidHelper() ? il2cpp_class_get_method_from_name(lm_helper.get_cls(), "Localize", 2) : nullptr;
  if (!ctx_ctor || !localize_m) {
    return "";
  }
  static auto instance_prop = lm_helper.GetParent("MonoSingleton`1").GetProperty("Instance");
  auto        lm            = instance_prop.GetRaw<void>(nullptr);
  if (!lm) {
    return "";
  }
  auto             ctx          = il2cpp_object_new(ctx_helper.get_cls());
  void*            ctor_args[2] = {il2cpp_string_new(identifier.c_str()), il2cpp_string_new(category.c_str())};
  Il2CppException* ex           = nullptr;
  il2cpp_runtime_invoke(ctx_ctor, ctx, ctor_args, &ex);
  if (ex) {
    return "";
  }
  if (update_hashes) {
    il2cpp_runtime_invoke(update_hashes, ctx, nullptr, &ex);
    ex = nullptr;
  }
  Il2CppString* out    = nullptr;
  void*         args[] = {&out, ctx};
  auto          r      = il2cpp_runtime_invoke(localize_m, lm, args, &ex);
  std::string   text;
  if (!ex && r && *(bool*)il2cpp_object_unbox(r) && out) {
    text = to_string(out);
  }
  return text;
}

static std::string localize(const std::string& category, const std::string& identifier)
{
  static std::unordered_map<std::string, std::string> cache;
  const auto key = category + "\t" + identifier;
  if (auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }
  // key format learned from the game UI: officer_name_<locaId> / resource_name_<locaId>
  std::vector<std::string> variants;
  if (category == "officer_names") {
    variants = {"officer_name_" + identifier, identifier, "officer_names_" + identifier};
  } else if (category == "materials") {
    variants = {"resource_name_" + identifier, identifier, "resource_name_short_" + identifier};
  } else if (category == "research") {
    variants = {"research_project_name_" + identifier, identifier};
  } else if (category == "research_trees") {
    variants = {"research_tree_name_" + identifier, "research_tree_" + identifier, identifier};
  } else if (category == "ships") {
    variants = {"ship_name_" + identifier, "ship_common_name_" + identifier, identifier};
  } else if (category == "ship_components") {
    variants = {"component_name_" + identifier, identifier};
  } else if (category == "factions") {
    variants = {"faction_name_" + identifier, identifier};
  } else {
    variants = {identifier};
  }
  std::string text;
  for (const auto& v : variants) {
    text = localize_one(category, v);
    if (!text.empty()) break;
  }
  if (cache.size() < 5000) {
    cache[key] = text;
  }
  return text;
}

// --- image url -> cache file map -------------------------------------------------------------
// The game downloads portraits/cards to DownloadCacheManager/<sha1>.png. Hooking its file-name
// function gives us url -> file pairs; the viewer matches officers/ships by art ref inside the url.
#define FILE_DEF_IMAGES "community_patch_images.json"
static std::unordered_map<std::string, std::string> image_map;
static std::mutex                                   image_mtx;
static bool                                         image_dirty = false;

static Il2CppString* DownloadCacheManager_GenerateFilenameFromURL_Hook(auto original, Il2CppString* url,
                                                                       bool compressed)
{
  auto result = original(url, compressed);
  if (url && result) {
    std::scoped_lock lk(image_mtx);
    auto             u = to_string(url);
    if (image_map.size() < 20000 && !image_map.contains(u)) {
      image_map[u] = to_string(result);
      image_dirty  = true;
    }
  }
  return result;
}

// Player avatars are flat officer portraits (DownloadCacheManager avatar_texture_<id>.png).
// Resolve avatar id -> display name once via the game's translator so the viewer can match officers by name.
#define FILE_DEF_AVATARS "community_patch_avatars.json"
static void write_avatar_names()
{
  static bool done = false;
  if (done) {
    return;
  }
  // avatar ids come from the urls the game hashed (avatar_texture_<id>.png); ids are sparse, up to ~100000
  std::vector<std::string> ids;
  {
    std::scoped_lock lk(image_mtx);
    for (const auto& [url, file] : image_map) {
      auto p = url.rfind("avatar_texture_");
      if (p == std::string::npos) continue;
      auto q = url.find(".png", p);
      if (q == std::string::npos) continue;
      auto id = url.substr(p + 15, q - (p + 15));
      if (!id.empty() && id.find('_') == std::string::npos) ids.push_back(id);
    }
  }
  if (ids.size() < 50) {
    return;  // image map not filled yet; try again next tick
  }
  nlohmann::json j;
  int            hits = 0;
  for (const auto& id : ids) {
    auto name = localize("player_avatars", "avatar_name_" + id);
    if (!name.empty()) {
      j[id] = name;
      ++hits;
    }
  }
  if (hits < 50) {
    return;  // translator not ready yet; try again next tick
  }
  done = true;
  const std::string path = std::string(File::ExportPath(FILE_DEF_AVATARS));
  std::ofstream     f(path, std::ios::trunc);
  f << j.dump();
  spdlog::info("Fleet export: {} avatar names written", hits);
}

static void write_image_map()
{
  std::scoped_lock lk(image_mtx);
  if (!image_dirty) {
    return;
  }
  image_dirty = false;
  nlohmann::json j = image_map;
  const std::string path = std::string(File::ExportPath(FILE_DEF_IMAGES));
  const std::string tmp  = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << j.dump();
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
}

static void InstallImageMapHook()
{
  auto helper = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.Storage", "DownloadCacheManager");
  if (!helper.isValidHelper()) {
    ErrorMsg::MissingHelper("Storage", "DownloadCacheManager");
    return;
  }
  auto ptr = helper.GetMethod("GenerateFilenameFromURL", 2);
  if (!ptr) {
    ErrorMsg::MissingMethod("DownloadCacheManager", "GenerateFilenameFromURL");
    return;
  }
  SPUD_STATIC_DETOUR(ptr, DownloadCacheManager_GenerateFilenameFromURL_Hook);
}

// Start Yeoman.exe at game start when Yeoman has written its path into [yeoman] exe.
// ponytail: no running-check here; Yeoman exits on its own when its port is already taken.
static void LaunchYeoman()
{
  const auto& exe = Config::Get().yeomanExe;
  if (exe.empty()) {
    return;
  }
  if (!std::filesystem::exists(exe)) {
    spdlog::warn("Yeoman: exe not found, not starting it: {}", exe);
    return;
  }
  auto dir = std::filesystem::path(exe).parent_path().string();
  auto r   = (INT_PTR)ShellExecuteA(nullptr, "open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
  if (r > 32) {
    spdlog::info("Yeoman: started {}", exe);
  } else {
    spdlog::warn("Yeoman: could not start {} (error {})", exe, r);
  }
}

void InstallFleetExportHooks()
{
  LaunchYeoman();
  InstallImageMapHook();
  auto helper = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.UI", "ScreenManager");
  if (!helper.isValidHelper()) {
    ErrorMsg::MissingHelper("UI", "ScreenManager");
    return;
  }
  auto ptr = helper.GetMethod("LateUpdate");
  if (!ptr) {
    ErrorMsg::MissingMethod("ScreenManager", "LateUpdate");
    return;
  }
  SPUD_STATIC_DETOUR(ptr, ScreenManager_LateUpdate_Hook);
  spdlog::info("Fleet export: writing {} every 3s", FILE_DEF_FLEETS);
}
