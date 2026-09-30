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

// Write `state` to a JSON file at `path`. The session JSON holds
// every chase's authoring data; the AE comps are produced by the
// in-process AEGP builder (ae_build), not by any sidecar file.
// Sets state.last_error on failure.
bool WriteSession(PanelState* state, const std::string& path);

// Load a session JSON from `path`. Replaces existing state, starts
// re-scans for each source, restores binds/tags/chases. Sets
// state.last_error on failure.
bool LoadSession(PanelState* state, const std::string& path);

// Reset to a freshly-opened panel: sources, chases, tags, binds,
// overrides, review-tab caches and undo history all cleared. Per-user
// preferences and the project FPS pin are deliberately kept — they
// describe the artist, not the scene.
void NewSession(PanelState* state);

// The session as JSON text, without touching a file. Used by the
// AE-project store (ae_build) so the .aep can carry the session.
std::string SerializeSession(PanelState* state);

// Load from JSON text rather than from a file. `path` is remembered as
// the save default and may be empty, which is the case when the text
// came out of the AE project. Same semantics as LoadSession otherwise:
// replaces state, starts re-scans, sets state.last_error on failure.
bool LoadSessionText(PanelState* state, const std::string& text,
                     const std::string& path);

// ===== User preferences ===============================================
//
// A handful of UI settings that belong to the PERSON, not to a scene:
// re-picking them every time a new scene is imported is pure friction.
// Kept in a tiny file under the user's app-data directory, separate
// from any session, and best-effort throughout — a missing or corrupt
// prefs file just means defaults, never an error the artist has to
// deal with.
void LoadPrefs(PanelState* state);
void SavePrefs(const PanelState* state);

} // namespace session_io
