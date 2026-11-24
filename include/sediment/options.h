#pragma once

namespace sediment {

// Settings for DB::Open. Each setting is added with the feature it controls,
// so that every field here has an effect.
struct Options {
  // If true, Put and Delete sync the write-ahead log before returning, so an
  // acknowledged write survives power loss. If false, an acknowledged write
  // survives a process crash but may be lost if the machine stops.
  bool sync_writes = false;
};

}  // namespace sediment
