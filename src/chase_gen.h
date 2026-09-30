// Chase generation + preview compositing — the pure logic behind the
// panel, with no ImGui and no AE dependency.
//
// Everything here is a plain function over PanelState / Chase, so it
// can be exercised by the native test target (tests/) without AE, a
// GPU, or a UI. Keep it that way: if a function needs ImGui, it
// belongs in panel_ui.cpp, not here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "panel_state.h"

namespace chase_gen {

// ===== The two timing knobs ===========================================
//
// One per kind of chase, each in the terms the artist uses:
//
//   Sweeps  -> HOW LONG THE CLIP IS. The stagger between lights is
//              derived, so adding lights tightens the spacing instead
//              of dragging the chase out.
//   Random  -> HOW MANY LIGHTS ARE LIT AT A TIME. The number of copies
//              of each light is derived from it.
//
// Both used to be stated the other way round, which left the artist
// doing this arithmetic by hand — the density that looks right differs
// for a 6-light rig and a 126-light one.

// Default length of a non-looping sweep, end to end. David: "the
// non-random chases are usually something like 4 seconds."
inline constexpr float kDefaultSweepSeconds = 4.f;

// Default number of lights lit at once in a Random chase.
inline constexpr float kDefaultLightsOn = 3.f;

// The 10-second loop is a convention carried over from the manual
// workflow (duplicate the light layers, scatter them, cut for a
// seamless 10s loop). Chase::loop_seconds already defaults to it.
inline constexpr float kConventionalLoopSeconds = 10.f;

// 3 Step's default spacing: how long after one chunk fires before the
// next one does. David's number. Not derived from the hit — the hit is
// whatever preset was picked, and this is the separate per-scene knob.
inline constexpr float kDefaultChunkStepFrames = 10.f;

// ===== Hit envelope presets ===========================================
//
// Noah's Kbar buttons ("NWE Gamma Light Hit 15/30/60/90"), decoded from
// the .ffx files. All four are Gamma Correction 0.25 -> 1 -> 0.25 over
// Opacity 0 -> 100 -> 0; they differ in length AND shape — the short
// ones punch and decay, the long ones rise and fall evenly. A fresh
// ChaseTiming already IS Hit 30.
struct HitPreset {
    const char* name;
    float       duration;      // frames
    float       attack;        // frames to the peak
};
inline const HitPreset kHitPresets[] = {
    { "Hit 15", 15.f,  4.f },
    { "Hit 30", 30.f,  5.f },
    { "Hit 60", 60.f, 30.f },
    { "Hit 90", 90.f, 45.f },
};
inline constexpr int kHitPresetCount = 4;
inline constexpr int kDefaultHitPreset = 1;      // Hit 30

// Set the hit shape, leaving pacing and look (step, opacity, gamma)
// untouched.
void ApplyHitPreset(ChaseTiming& t, int index);

// Which preset a timing is closest to, for showing the current choice.
int NearestHitPreset(const ChaseTiming& t);

// ===== Templates ======================================================

// Wizard/pack template names. Index order is persisted in sessions
// via the chase fields ApplyTemplateToChase writes — append only.
inline const char* const kChaseTemplateNames[] = {
    "Left to Right",
    "Top to Bottom",
    "Center Out",
    "3 Step Chase",
    "Random",
    "Hit",
    "Custom",
};
inline constexpr int kChaseTemplateCount =
    static_cast<int>(sizeof(kChaseTemplateNames) /
                     sizeof(kChaseTemplateNames[0]));

// Apply a template's sort / staging / scatter defaults to `c`.
void ApplyTemplateToChase(Chase& c, int template_idx);

// ===== Export naming ==================================================

// "Left to Right" -> "LeftToRight"; "3 Step Chase" -> "3StepChase".
// Keeps letters and digits and drops everything else, capitalising the
// letter after each break so a spaced name reads as CamelCase rather
// than running together. Underscore is the only separator in a
// delivery name ("USC_CatTowers_LeftToRight_v1"), so every part has to
// lose its own spaces and punctuation first.
std::string CompactExportName(const std::string& s);

// ===== Contact sheet ==================================================

// Where each chase sits in the generated grid. `count` cells are
// filled, laid out left-to-right / top-to-bottom; cell k covers
// [ (k%cols)*cell_w, (k/cols)*cell_h ] and is drawn from that
// sub-rectangle of the single sheet texture.
struct ContactSheetLayout {
    int count   = 0;
    int cols    = 0;
    int rows    = 0;
    int cell_w  = 0;
    int cell_h  = 0;
    int sheet_w = 0;
    int sheet_h = 0;
};

// Composite every chase into ONE tiled buffer — every cell previewing
// at the same moment, uploaded as a single texture. Deliberately not
// one texture per cell: PanelState carries exactly one composite
// texture and both renderers hand-manage its lifecycle, so a pool of
// them would mean reopening the panel-teardown code behind the AE
// crash saga.
//
// `playheads` gives each chase its OWN position in frames — one entry
// per chase, wrapped to that chase's length. Chases in a set are
// different durations, so a single shared playhead reads as arbitrary:
// a scrub that means "a third of the way" through a 2-second chase is
// somewhere unrelated in a 20-second one. Each cell therefore carries
// its own transport. Short lists are padded with 0.
//
// (Two earlier attempts were wrong: normalising one position to each
// chase's own length made every chase take the same time to play, and
// one shared frame count let long chases run while short ones spun.)
ContactSheetLayout BuildContactSheet(const std::vector<Chase>& chases,
                                     const PanelState& state,
                                     const std::vector<float>& playheads,
                                     float fps, int cell_w, int cols,
                                     std::vector<uint8_t>& out_rgba);

// The longest chase in the set, in frames — the span a shared transport
// has to cover for every chase to have played through at least once.
float LongestChaseFrames(const std::vector<Chase>& chases,
                         const PanelState& state, float fps);

// ===== Random scatter =================================================

// Regenerate a Random chase's hit list: every eligible light fires
// `scatter_density` times across the loop, placed so the lit
// brightness stays even (no clumps of bright lights, no dark holes)
// while still reading as random. Deterministic from `random_seed`.
void RegenerateScatter(Chase& chase, const PanelState& state, float fps);

// ===== Standard pack ==================================================

// The five chases a show actually needs, generated in one go:
// Left to Right, Bottom to Top, Center Out, 3 Step Chase, Random.
// No reverse variants — those are done on the playback server, so a
// separate comp per direction is waste.
//
// `tag_ids` SCOPES the pack; it does not multiply it. An empty list is
// the whole scene; a list of tags means "build these five chases out of
// those families' lights". Checking three tags gives five chases, not
// fifteen. Chase names are the plain template names whatever the scope,
// so a regenerated pack matches the previous one by name and its timing
// carries over.
//
// Every chase gets the session's current hit envelope (the chosen Hit
// preset). Sweeps are then fitted to `sweep_seconds` end to end, and
// Random's density is derived from `lights_on` — so a fresh pack is
// already in the right ballpark for the scene's size.
//
// Returns how many chases were appended (0 if the session has no
// eligible lights).
int GenerateStandardPack(PanelState& state,
                         const std::vector<uint32_t>& tag_ids,
                         float fps,
                         float lights_on = kDefaultLightsOn,
                         float sweep_seconds = kDefaultSweepSeconds);

// Re-apply the per-chase tweaks from `previous` onto same-named chases
// in `state`, then rebuild their stages / scatter so the new light
// membership honours them.
//
// This is what makes the Include/Exclude round trip non-destructive:
// generate, tune the hits and lengths in Review, notice a light that
// shouldn't be in the set, go back, untick it, generate again — and the
// tuning survives. Matching is BY NAME, which works because a replaced
// pack regenerates the same names for the same scopes.
//
// Carried: the hit envelope and pacing (`timing`), scatter density and
// seed, the sort basis, the chunk count, the loop settings. NOT carried:
// identity (id, name, tag filter) or membership (stages, scatter,
// manual arrangement) — new membership is the entire point of
// regenerating.
//
// Returns how many chases matched a previous one.
int CarryOverChaseTweaks(PanelState& state,
                         const std::vector<Chase>& previous, float fps);

// How many logical lights a scope actually drives: included, dedupe-
// surviving, tag-matching, with binds counted once. Tag members whose
// source is gone (a source removed and re-added mints a new id, and
// nothing prunes the old refs) resolve to nothing and are not counted,
// so this is the honest "N lights" to show before generating.
int CountEligibleLights(const PanelState& state,
                        const std::vector<uint32_t>& tag_filter);

// What a finished timing actually delivers: duration / step, i.e. the
// average lights lit. Differs from the requested `lights_on` when the
// loop is too short for the light count and the hit hits its floor.
float AchievedLightsOn(const ChaseTiming& t);

// Drop references to sources that are no longer in the session, from
// tags, binds, chase stages and scatter hits. Removing a source and
// re-adding it mints a new source_id, and nothing used to clean up the
// refs left behind — a production session was found with 143 of 293 tag
// members orphaned. They are inert, but they inflate every count the UI
// shows.
//
// Deliberately conservative. A reference into a source that IS present
// but scanned empty is KEPT — deleting it would destroy the user's
// grouping over a temporary read failure.
//
// **A failed EXR scan adds no Source at all** (exr_scan's scan worker
// only records the error), so on the load path "absent from
// state.sources" does NOT mean "gone from the project" — it may just
// mean the file moved. Loading therefore passes the source ids the
// SESSION FILE listed, which is the honest answer to what belongs to
// this project; the no-argument form uses the sources currently in
// state and is for an explicit user-initiated cleanup.
//
// Emptied tags and binds are kept; emptied chase stages are dropped,
// since a memberless stage would just add a silent gap to the running
// order. Returns how many references were removed.
int PruneOrphanedRefs(PanelState& state,
                      const std::vector<uint32_t>& live_source_ids);
int PruneOrphanedRefs(PanelState& state);

// Sort the whole scene, not one file of it.
//
// SortLayers orders the layers inside ONE source, which was the whole
// job when a scene was a single multilayer EXR. A show rendered one
// clip per light is 138 sources holding one layer each, and sorting a
// one-element vector 138 times leaves the list in load order — picking
// "Left to Right" appeared to do nothing.
//
// So: sort each source's layers, then order the SOURCES by their
// leading light. For one-light-per-file scenes that is an exact global
// ordering. For a mixed scene (a big EXR plus a few extra files) each
// source stays internally sorted and the sources are ordered by where
// they start — better than load order, though not a true interleave;
// that needs Staging to carry a display order separate from storage
// order, which is a bigger change.
//
// `active_source_index` is carried across the reorder, and sources with
// no layers sort to the end.
void SortAllSources(PanelState& state, SortMode mode, bool reverse,
                    uint32_t seed);

// ===== Stage generation ===============================================

// Recompute the chase's stages from its sort + tag filter + the
// session's staged layers.
void RegenerateChaseStages(Chase& chase, const PanelState& state,
                           int stages_count_hint);

// ===== Fitting the timing =============================================

// Sweeps: derive the stagger from how long the whole clip should be.
// `sweep_seconds` is the end-to-end length of the chase; the step falls
// out of it, so adding lights tightens the spacing instead of dragging
// the chase out. The hit envelope on `base` (duration/attack, i.e. the
// chosen Hit preset) is carried through untouched — length of clip and
// shape of hit are separate decisions.
//
// In LOOP mode (`loop_frames > 0`, an animation source) the scene's own
// loop dictates the spacing instead: stages spread across it so the
// chase wraps seamlessly, and `sweep_seconds` does not apply.
ChaseTiming FitSweepTiming(int n_stages, int loop_frames, int cycles,
                           float fps, ChaseTiming base,
                           float sweep_seconds = kDefaultSweepSeconds);

// Chunked chases (3 Step): the chunks use the SAME hit presets as
// everything else, and the next chunk triggers as the previous one
// starts to fade. That moment is the hit's attack, so:
//
//     step = attack        (of the chosen Hit preset)
//
// which means a chunked chase has no clip-length knob — its length
// falls out of the hit: (chunks - 1) * attack + duration. Picking a
// different Hit changes the whole feel: Hit 60 (attack 30 of 60) is a
// broad cross-fade, Hit 30 (attack 5 of 30) fires in quick succession
// with long overlapping tails. Nothing ever goes fully dark, because
// the next chunk is already up before the previous one is out.
ChaseTiming FitChunkedTiming(int n_stages, int loop_frames, int cycles,
                             float fps, ChaseTiming base,
                             float clip_seconds = kDefaultSweepSeconds);

// Random: how many copies of each light to scatter so that about
// `lights_on` are lit at any moment. This is the arithmetic the artist
// used to do in their head — the density that reads right on a 6-light
// rig is not the one for 126 lights.
//
//     density = lights_on * loop_frames / (n_lights * hit_frames)
//
// Never returns less than 1: every light fires at least once per loop.
// A big rig with long hits therefore has a FLOOR on how sparse it can
// be — 126 lights with 1-second hits in a 10-second loop already keeps
// ~13 lit. Callers should report what was actually achieved rather than
// pretend the request was met.
float DensityForLightsOn(float lights_on, int n_lights, int loop_frames,
                       float hit_frames);

// How many lights a finished scatter actually keeps lit, so the UI can
// show what came out when the request could not be met.
float ScatterLightsOn(float density, int n_lights, int loop_frames,
                      float hit_frames);

// ===== Envelopes ======================================================

// Wrapped (seamless-loop) triangle envelope for one hit.
float ScatterHitEnvelope(float playhead, float start, float loop_frames,
                         const ChaseTiming& t);

// Non-loop triangle envelope for a stage (shift-in-time mode).
float StageEnvelope(int stage_idx, float playhead, const ChaseTiming& t);

// Total chase length in frames when the last stage's release ends.
float ChaseTotalDuration(const Chase& chase);

// Per-stage envelope honoring loop-mode.
float ChaseStageEnvelopeAt(const Chase& chase, size_t si, float playhead,
                           int loop_frames);

// Loop-mode-aware total preview length in frames.
float ChaseTotalFrames(const Chase& chase, const PanelState& state,
                       float fps);

// ===== Preview compositing ============================================

// One weighted layer going into a composite. `k` is the envelope
// multiplier (opacity * gamma_at_t), applied linearly.
struct CompositeContribution { const LayerInfo* L; float k; };

// Lighten-composite weighted contributions, matching the AE build
// (black solid + Lighten — per-channel max, NOT additive).
bool LightenComposite(const std::vector<CompositeContribution>& contribs,
                      int out_w, int out_h, float shared_peak,
                      std::vector<uint8_t>& out_rgba);

bool BuildChaseComposite(const Chase& chase, const PanelState& state,
                         float playhead,
                         std::vector<uint8_t>& out_rgba,
                         int& out_w, int& out_h);

bool BuildRefsComposite(const std::vector<LayerRef>& refs,
                        const PanelState& state,
                        std::vector<uint8_t>& out_rgba,
                        int& out_w, int& out_h);

bool BuildScatterComposite(const Chase& chase, const PanelState& state,
                           float playhead, float fps,
                           std::vector<uint8_t>& out_rgba,
                           int& out_w, int& out_h);

} // namespace chase_gen
