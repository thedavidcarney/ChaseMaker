// Session save/load: serialise PanelState's session-level data
// (sources, binds, tags, position overrides, chases, timing
// defaults) to / from a .chasemaker.json sidecar.
//
// Layer data is NOT saved — only source paths. On load, each source
// is re-scanned from disk so the layer metrics are always fresh
// against the actual file contents. LayerRefs survive because they
// key off (source_id, FNV hash of layer name); the source_id is
// preserved on save and reasserted on load.

#pragma once

#include <string>

struct PanelState;

namespace session_io {

// Write `state` to a JSON file at `path`. Also writes one
// `<dir>/<basename>.<chase_name>.chase.json` per chase, the format
// the downstream JSX executor will read to build the AE comp.
// Sets state.last_error on failure.
bool WriteSession(PanelState* state, const std::string& path);

// Load a session JSON from `path`. Replaces existing state, starts
// re-scans for each source, restores binds/tags/chases. Sets
// state.last_error on failure.
bool LoadSession(PanelState* state, const std::string& path);

} // namespace session_io
