#include "ae_build.h"

#include "panel_state.h"
#include "hash.h"

#include "AEConfig.h"
#include "AE_GeneralPlug.h"
#include "AE_EffectCB.h"        // PF_Xfer_LIGHTEN
#include "AEGP_SuiteHandler.h"
#include "SPBasic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ae_build {

namespace {

// ===== UTF conversions ================================================

std::vector<A_UTF16Char> ToUtf16(const std::string& utf8)
{
    std::vector<A_UTF16Char> out;
    out.reserve(utf8.size() + 1);
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80)            { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else                          { cp = '?';      extra = 0; }
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        }
        if (cp < 0x10000) {
            out.push_back(static_cast<A_UTF16Char>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<A_UTF16Char>(0xD800 | (cp >> 10)));
            out.push_back(static_cast<A_UTF16Char>(0xDC00 | (cp & 0x3FF)));
        }
    }
    out.push_back(0);
    return out;
}

std::string FromUtf16(const A_UTF16Char* utf16)
{
    std::string out;
    if (!utf16) return out;
    for (size_t i = 0; utf16[i] != 0; ++i) {
        uint32_t cp = utf16[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && utf16[i + 1] != 0) {
            uint32_t low = utf16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                ++i;
            }
        }
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Pull a UTF-8 string from an AEGP MemHandle of UTF-16 chars,
// disposing the handle on return. Empty string if handle is null.
std::string MemHandleToString(AEGP_SuiteHandler& sp, AEGP_MemHandle h)
{
    if (!h) return {};
    A_UTF16Char* p = nullptr;
    sp.MemorySuite1()->AEGP_LockMemHandle(h, reinterpret_cast<void**>(&p));
    std::string out = p ? FromUtf16(p) : std::string{};
    sp.MemorySuite1()->AEGP_UnlockMemHandle(h);
    sp.MemorySuite1()->AEGP_FreeMemHandle(h);
    return out;
}

// ===== Project + folder helpers =======================================

// Returns the folder we drop the new ChaseMaker_vNN folder into:
// ALWAYS the project root. We deliberately ignore the Project-panel
// selection — honoring it meant that building while a prior
// ChaseMaker comp/folder was selected nested the new folder inside
// the old one, which David didn't want. Versioning still applies, so
// repeated builds land as ChaseMaker, ChaseMaker_v02, ... all at the
// top level.
A_Err FindTargetFolder(AEGP_SuiteHandler& sp,
                       AEGP_ProjectH proj,
                       AEGP_ItemH* out_folder)
{
    *out_folder = nullptr;
    return sp.ProjSuite6()->AEGP_GetProjectRootFolder(proj, out_folder);
}

// Inside `parent`, find an item with given UTF-8 name. Returns
// nullptr in *out_item if not found. type_filter limits the search;
// pass AEGP_ItemType_NONE to match any type.
A_Err FindChildByName(AEGP_SuiteHandler& sp,
                      AEGP_ProjectH proj,
                      AEGP_ItemH parent,
                      const std::string& name,
                      AEGP_ItemType type_filter,
                      AEGP_PluginID plugin_id,
                      AEGP_ItemH* out_item)
{
    *out_item = nullptr;
    AEGP_ItemH it = nullptr;
    sp.ItemSuite9()->AEGP_GetFirstProjItem(proj, &it);
    while (it) {
        AEGP_ItemH it_parent = nullptr;
        sp.ItemSuite9()->AEGP_GetItemParentFolder(it, &it_parent);
        if (it_parent == parent) {
            AEGP_ItemType t = AEGP_ItemType_NONE;
            sp.ItemSuite9()->AEGP_GetItemType(it, &t);
            if (type_filter == AEGP_ItemType_NONE || t == type_filter) {
                AEGP_MemHandle hname = nullptr;
                if (!sp.ItemSuite9()->AEGP_GetItemName(plugin_id, it, &hname)) {
                    std::string nm = MemHandleToString(sp, hname);
                    if (nm == name) {
                        *out_item = it;
                        return A_Err_NONE;
                    }
                }
            }
        }
        AEGP_ItemH next = nullptr;
        sp.ItemSuite9()->AEGP_GetNextProjItem(proj, it, &next);
        it = next;
    }
    return A_Err_NONE;
}

// ===== Footage import =================================================

// Case-insensitive, separator-normalized path compare (Windows paths
// from AE vs our std::filesystem paths can differ in slash/case).
std::string NormPath(const std::string& s)
{
    std::string o = s;
    for (char& c : o) {
        if (c == '/') c = '\\';
        else c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
    }
    return o;
}

// Find a footage item already in the project whose source path
// matches `path` OR `alt_path` (the latter lets us reuse a sequence
// the user imported themselves — AE reports its path as frame 0,
// which is our scan_path). Returns nullptr in *out_item if not found.
A_Err FindFootageByPath(AEGP_SuiteHandler& sp,
                        AEGP_ProjectH proj,
                        AEGP_PluginID plugin_id,
                        const std::string& path,
                        const std::string& alt_path,
                        AEGP_ItemH* out_item)
{
    *out_item = nullptr;
    const std::string want  = NormPath(path);
    const std::string want2 = alt_path.empty() ? std::string()
                                               : NormPath(alt_path);
    AEGP_ItemH it = nullptr;
    sp.ItemSuite9()->AEGP_GetFirstProjItem(proj, &it);
    while (it) {
        AEGP_ItemType t = AEGP_ItemType_NONE;
        sp.ItemSuite9()->AEGP_GetItemType(it, &t);
        if (t == AEGP_ItemType_FOOTAGE) {
            AEGP_FootageH fh = nullptr;
            if (!sp.FootageSuite5()->AEGP_GetMainFootageFromItem(it, &fh) && fh) {
                AEGP_MemHandle hpath = nullptr;
                if (!sp.FootageSuite5()->AEGP_GetFootagePath(
                        fh, 0, AEGP_FOOTAGE_MAIN_FILE_INDEX, &hpath))
                {
                    std::string p = NormPath(MemHandleToString(sp, hpath));
                    if (p == want || (!want2.empty() && p == want2)) {
                        *out_item = it;
                        return A_Err_NONE;
                    }
                }
            }
        }
        AEGP_ItemH next = nullptr;
        sp.ItemSuite9()->AEGP_GetNextProjItem(proj, it, &next);
        it = next;
    }
    return A_Err_NONE;
}

// Find or import the source EXR. The newly-imported item lands in
// the chasemaker_folder so users can find it.
A_Err EnsureFootage(AEGP_SuiteHandler& sp,
                    AEGP_ProjectH proj,
                    AEGP_PluginID plugin_id,
                    const std::string& path,
                    const std::string& scan_path,
                    bool as_sequence,
                    AEGP_ItemH chasemaker_folder,
                    AEGP_ItemH* out_footage)
{
    A_Err err = FindFootageByPath(sp, proj, plugin_id, path,
                                  scan_path, out_footage);
    if (err) return err;
    if (*out_footage) return A_Err_NONE;
    // Not found — import. For an animation source import the whole
    // image sequence (a concrete frame path + all_in_folder) so the
    // built comp animates; otherwise import the path as-is (still).
    AEGP_FootageH footH = nullptr;
    const std::string import_path =
        (as_sequence && !scan_path.empty()) ? scan_path : path;
    std::vector<A_UTF16Char> u16 = ToUtf16(import_path);
    AEGP_FileSequenceImportOptions seq{};
    if (as_sequence) {
        seq.all_in_folderB     = TRUE;
        seq.force_alphabeticalB = TRUE;
        seq.start_frameL       = AEGP_ANY_FRAME;
        seq.end_frameL         = AEGP_ANY_FRAME;
    }
    err = sp.FootageSuite5()->AEGP_NewFootage(
        plugin_id, u16.data(),
        nullptr,  // no layer key — import as merged
        as_sequence ? &seq : nullptr,
        AEGP_InterpretationStyle_NO_DIALOG_GUESS,
        nullptr,  // reserved
        &footH);
    if (err || !footH) return err;
    return sp.FootageSuite5()->AEGP_AddFootageToProject(
        footH, chasemaker_folder, out_footage);
}

// ===== Effect helpers =================================================

// Find an installed effect by exact match name (e.g.
// "tdcarney EXRDemux"). Returns AEGP_InstalledEffectKey_NONE if not
// found.
A_Err FindInstalledEffectKey(AEGP_SuiteHandler& sp,
                             const char* match_name_utf8,
                             AEGP_InstalledEffectKey* out_key)
{
    *out_key = AEGP_InstalledEffectKey_NONE;
    AEGP_InstalledEffectKey key = AEGP_InstalledEffectKey_NONE;
    while (true) {
        AEGP_InstalledEffectKey next = AEGP_InstalledEffectKey_NONE;
        if (sp.EffectSuite5()->AEGP_GetNextInstalledEffect(key, &next)) break;
        if (next == AEGP_InstalledEffectKey_NONE) break;
        char mn[AEGP_MAX_EFFECT_MATCH_NAME_SIZE] = {0};
        sp.EffectSuite5()->AEGP_GetEffectMatchName(next, mn);
        if (std::strcmp(mn, match_name_utf8) == 0) {
            *out_key = next;
            return A_Err_NONE;
        }
        key = next;
    }
    return A_Err_NONE;
}

// Sets a float-slider effect-param stream to a single value. Used
// for Layer Hash Hi / Lo. The param is CANNOT_TIME_VARY so a plain
// SetStreamValue is correct (no keyframe).
A_Err SetEffectFloatParam(AEGP_SuiteHandler& sp,
                          AEGP_PluginID plugin_id,
                          AEGP_EffectRefH effect,
                          PF_ParamIndex param_index,
                          double value)
{
    AEGP_StreamRefH stream = nullptr;
    A_Err err = sp.StreamSuite6()->AEGP_GetNewEffectStreamByIndex(
        plugin_id, effect, param_index, &stream);
    if (err || !stream) return err ? err : A_Err_GENERIC;
    AEGP_StreamValue2 val{};
    val.streamH = stream;
    val.val.one_d = value;
    err = sp.StreamSuite6()->AEGP_SetStreamValue(plugin_id, stream, &val);
    sp.StreamSuite6()->AEGP_DisposeStream(stream);
    return err;
}

// Find an effect param stream whose display name contains a given
// substring. Used to locate "Gamma Correction" inside the Exposure
// effect without having to hardcode its param index (which varies).
// Returns the first match; caller must dispose the returned stream.
A_Err FindEffectStreamByDisplayName(AEGP_SuiteHandler& sp,
                                    AEGP_PluginID plugin_id,
                                    AEGP_EffectRefH effect,
                                    const std::string& needle,
                                    AEGP_StreamRefH* out_stream)
{
    *out_stream = nullptr;
    A_long n = 0;
    sp.StreamSuite6()->AEGP_GetEffectNumParamStreams(effect, &n);
    // Param index 0 is the effect's implicit input-layer stream. AE
    // raises an internal-verification-failure dialog if you actually
    // pass index 0 (the header comment claiming "[0..N-1]" lies).
    // Start at 1 so we only ask about user-facing params.
    for (A_long i = 1; i < n; ++i) {
        AEGP_StreamRefH s = nullptr;
        if (sp.StreamSuite6()->AEGP_GetNewEffectStreamByIndex(
                plugin_id, effect, i, &s) || !s) continue;
        AEGP_MemHandle hname = nullptr;
        sp.StreamSuite6()->AEGP_GetStreamName(plugin_id, s, false, &hname);
        std::string display = MemHandleToString(sp, hname);
        if (display.find(needle) != std::string::npos) {
            *out_stream = s;
            return A_Err_NONE;
        }
        sp.StreamSuite6()->AEGP_DisposeStream(s);
    }
    return A_Err_NONE;
}

// Bake three keyframes (linear-interp by default) on a one-D stream.
// Times are in layer-local seconds since the layer's start.
A_Err BakeThreeKeyframes(AEGP_SuiteHandler& sp,
                         AEGP_StreamRefH stream,
                         const A_Time& t0, double v0,
                         const A_Time& t1, double v1,
                         const A_Time& t2, double v2)
{
    if (!stream) return A_Err_GENERIC;
    AEGP_AddKeyframesInfoH ak = nullptr;
    A_Err err = sp.KeyframeSuite5()->AEGP_StartAddKeyframes(stream, &ak);
    if (err || !ak) return err ? err : A_Err_GENERIC;

    auto add_one = [&](const A_Time& t, double v) {
        A_long idx = 0;
        sp.KeyframeSuite5()->AEGP_AddKeyframes(ak, AEGP_LTimeMode_LayerTime,
                                               &t, &idx);
        AEGP_StreamValue2 val{};
        val.streamH = stream;
        val.val.one_d = v;
        sp.KeyframeSuite5()->AEGP_SetAddKeyframe(ak, idx, &val);
    };
    add_one(t0, v0);
    add_one(t1, v1);
    add_one(t2, v2);
    sp.KeyframeSuite5()->AEGP_EndAddKeyframes(TRUE, ak);
    return A_Err_NONE;
}

// Bake an arbitrary keyframe list (linear interp) on a 1-D stream.
// Used for the seamless-loop wrapped envelope, which needs more than
// three breakpoints once it crosses the loop boundary.
A_Err BakeKeyframes(AEGP_SuiteHandler& sp,
                    AEGP_StreamRefH stream,
                    const std::vector<std::pair<A_Time, double>>& kf)
{
    if (!stream || kf.empty()) return A_Err_GENERIC;
    AEGP_AddKeyframesInfoH ak = nullptr;
    A_Err err = sp.KeyframeSuite5()->AEGP_StartAddKeyframes(stream, &ak);
    if (err || !ak) return err ? err : A_Err_GENERIC;
    for (const auto& p : kf) {
        A_long idx = 0;
        sp.KeyframeSuite5()->AEGP_AddKeyframes(ak, AEGP_LTimeMode_LayerTime,
                                               &p.first, &idx);
        AEGP_StreamValue2 val{};
        val.streamH = stream;
        val.val.one_d = p.second;
        sp.KeyframeSuite5()->AEGP_SetAddKeyframe(ak, idx, &val);
    }
    sp.KeyframeSuite5()->AEGP_EndAddKeyframes(TRUE, ak);
    return A_Err_NONE;
}

// Convert "frames at fps" to A_Time. duration_in_seconds = frames/fps.
// A_Time models that as value/scale with value = frames*fps.den and
// scale = fps.num — exact rational, no float-to-rational conversion.
A_Time FramesToATime(double frames, A_Ratio fps)
{
    A_Time t{};
    if (fps.num <= 0) {
        t.value = 0;
        t.scale = 1;
        return t;
    }
    t.value = static_cast<A_long>(frames * fps.den + 0.5);
    t.scale = static_cast<A_u_long>(fps.num);
    return t;
}

// ===== Comp creation ==================================================

// A floating fps -> exact A_Ratio, preserving the common NTSC
// fractional rates and otherwise rounding to an integer/1.
A_Ratio RatioFromFps(A_FpLong fps)
{
    if (fps <= 0.01) return A_Ratio{24, 1};
    if (std::fabs(fps - 23.976) < 0.01) return A_Ratio{24000, 1001};
    if (std::fabs(fps - 29.97)  < 0.01) return A_Ratio{30000, 1001};
    if (std::fabs(fps - 59.94)  < 0.01) return A_Ratio{60000, 1001};
    return A_Ratio{static_cast<A_long>(fps + 0.5), 1};
}

// Pull the AE project's "natural" framerate. There's no direct
// project-level fps; pull from the active comp if any, else fall
// back to 24/1.
A_Ratio GetActiveCompFramerate(AEGP_SuiteHandler& sp)
{
    A_Ratio r{30, 1};   // no active comp -> default to 30 (the 99% case)
    AEGP_ItemH active = nullptr;
    sp.ItemSuite9()->AEGP_GetActiveItem(&active);
    if (active) {
        AEGP_ItemType t = AEGP_ItemType_NONE;
        sp.ItemSuite9()->AEGP_GetItemType(active, &t);
        if (t == AEGP_ItemType_COMP) {
            AEGP_CompH ch = nullptr;
            if (!sp.CompSuite11()->AEGP_GetCompFromItem(active, &ch) && ch) {
                A_FpLong fps = 0;
                sp.CompSuite11()->AEGP_GetCompFramerate(ch, &fps);
                if (fps > 0.01) return RatioFromFps(fps);
            }
        }
    }
    return r;
}

// ===== The build itself ===============================================

struct BuildContext {
    PanelState*               state = nullptr;
    AEGP_PluginID             plugin_id = 0;
    AEGP_ProjectH             proj = nullptr;
    AEGP_ItemH                target_folder = nullptr;
    AEGP_ItemH                chasemaker_folder = nullptr;
    std::string               chasemaker_folder_name;
    AEGP_ItemH                exr_footage = nullptr;
    // One shared black-solid item per build, sized to the source EXR.
    // Reused as the bottom layer of every chase comp so downstream
    // glow / blur effects always have an opaque backdrop, even on
    // frames where no chase light is active.
    AEGP_ItemH                black_solid = nullptr;
    AEGP_InstalledEffectKey   demux_key = AEGP_InstalledEffectKey_NONE;
    AEGP_InstalledEffectKey   exposure_key = AEGP_InstalledEffectKey_NONE;
    A_Ratio                   fps{24, 1};
    A_long                    src_w = 0;
    A_long                    src_h = 0;
    // Loop-mode (animation/sequence source): chases are seamless
    // loops of exactly `loop_frames` (the source's own duration);
    // every light's footage is time-locked (offset 0, spans the whole
    // comp) and only the opacity/exposure envelope sweeps + wraps.
    // 0 = still-image shift-in-time mode (legacy).
    int                       loop_frames = 0;
    int                       comps_created = 0;
    int                       layers_added = 0;
};

A_Err BuildOneChase(AEGP_SuiteHandler& sp,
                    const Chase& chase,
                    BuildContext& ctx)
{
    A_Err err = A_Err_NONE;

    const double fps_d = static_cast<double>(ctx.fps.num) /
        static_cast<double>(std::max<A_long>(1, ctx.fps.den));

    // Compute total duration in frames.
    //  - Loop-mode (animation source): EXACTLY the source duration, so
    //    the comp loops seamlessly and stays phase-locked to the scene.
    //  - Scatter (still source): its own seamless loop_seconds.
    //  - Stage chase (still source): runs until the last release.
    const bool loop_mode = (ctx.loop_frames > 0);
    float total_frames = 0.f;
    if (loop_mode) {
        total_frames = static_cast<float>(ctx.loop_frames);
    } else if (chase.random_scatter) {
        const double lf = static_cast<double>(
            std::llround(chase.loop_seconds * fps_d));
        total_frames = static_cast<float>(lf < 1.0 ? 1.0 : lf);
    } else if (!chase.stages.empty()) {
        total_frames = (chase.stages.size() - 1) * chase.timing.step_duration
                       + chase.timing.duration;
    }
    if (total_frames < 1.f) total_frames = 1.f;

    // A_Time duration:
    //   seconds = value / scale, want seconds = frames * den / num
    //   so value = frames * den, scale = num
    const A_long frames_i = static_cast<A_long>(total_frames + 0.5f);
    A_Time duration{};
    duration.value = frames_i * static_cast<A_long>(ctx.fps.den);
    duration.scale = static_cast<A_u_long>(ctx.fps.num);

    A_Ratio pix_aspect{1, 1};

    // Pick a comp name; suffix _vNN if collision.
    std::string comp_name = chase.name.empty() ? std::string{"Chase"} : chase.name;
    {
        AEGP_ItemH existing = nullptr;
        FindChildByName(sp, ctx.proj, ctx.chasemaker_folder,
                        comp_name, AEGP_ItemType_NONE,
                        ctx.plugin_id, &existing);
        if (existing) {
            for (int v = 2; v < 100; ++v) {
                char suffix[8];
                std::snprintf(suffix, sizeof(suffix), "_v%02d", v);
                std::string cand = comp_name + suffix;
                AEGP_ItemH ex = nullptr;
                FindChildByName(sp, ctx.proj, ctx.chasemaker_folder,
                                cand, AEGP_ItemType_NONE,
                                ctx.plugin_id, &ex);
                if (!ex) { comp_name = cand; break; }
            }
        }
    }

    std::vector<A_UTF16Char> u16name = ToUtf16(comp_name);
    AEGP_CompH new_comp = nullptr;
    err = sp.CompSuite11()->AEGP_CreateComp(
        ctx.chasemaker_folder, u16name.data(),
        ctx.src_w, ctx.src_h,
        &pix_aspect, &duration, &ctx.fps,
        &new_comp);
    if (err || !new_comp) return err ? err : A_Err_GENERIC;
    ++ctx.comps_created;

    // Get the new comp's item handle for AddLayer.
    AEGP_ItemH new_comp_item = nullptr;
    sp.CompSuite11()->AEGP_GetItemFromComp(new_comp, &new_comp_item);

    // Drop the shared black solid FIRST so subsequent chase-light
    // AddLayer calls stack above it — leaves the solid as the
    // bottommost layer, covering the whole comp duration.
    if (ctx.black_solid) {
        AEGP_LayerH bg_layer = nullptr;
        if (!sp.LayerSuite9()->AEGP_AddLayer(ctx.black_solid, new_comp, &bg_layer) &&
            bg_layer)
        {
            const A_Time bg_in = FramesToATime(0.0, ctx.fps);
            sp.LayerSuite9()->AEGP_SetLayerInPointAndDuration(
                bg_layer, AEGP_LTimeMode_LayerTime, &bg_in, &duration);
            // Leave blend mode at default (Normal) — the solid is the
            // opaque backdrop that Lighten-blended chase layers
            // composite onto.
        }
    }

    // Per-stage envelope keyframe times (in layer-local frames):
    //   t0 = 0                       (envelope start, opacity 0, gamma baseline)
    //   t1 = attack                  (peak, opacity peak%, gamma peak)
    //   t2 = duration                (envelope end, opacity 0, gamma baseline)
    const double attack_f   = std::max(0.0, std::min<double>(
        chase.timing.attack, chase.timing.duration));
    const double duration_f = std::max(1.0, (double)chase.timing.duration);
    const A_Time t0 = FramesToATime(0.0,        ctx.fps);
    const A_Time t1 = FramesToATime(attack_f,   ctx.fps);
    const A_Time t2 = FramesToATime(duration_f, ctx.fps);
    const A_Time layer_in_pt  = FramesToATime(0.0,        ctx.fps);
    const A_Time layer_dur    = FramesToATime(duration_f, ctx.fps);

    // Exact (no round-bias) frame->A_Time for layer offsets, which
    // can be negative for the seamless-loop wrap. FramesToATime's
    // +0.5 rounding skews negatives by a frame; this is exact.
    auto offset_at = [&](double frames) -> A_Time {
        A_Time t{};
        t.value = static_cast<A_long>(std::llround(frames)) *
                  static_cast<A_long>(ctx.fps.den);
        t.scale = static_cast<A_u_long>(ctx.fps.num);
        return t;
    };

    // Place one EXR layer for `display_name` at `offset` on the comp
    // timeline, trimmed to the envelope window, with the opacity +
    // exposure-gamma envelope baked. Shared by the stage path and the
    // random-scatter path.
    auto place_layer = [&](const std::string& display_name,
                           const A_Time& offset) {
        if (display_name.empty()) return;
        AEGP_LayerH layer = nullptr;
        if (sp.LayerSuite9()->AEGP_AddLayer(ctx.exr_footage, new_comp,
                                            &layer) || !layer)
            return;
        ++ctx.layers_added;

        // Name the timeline layer after the LIGHT (the footage
        // source still carries the filename) so per-light layers
        // are identifiable in the comp.
        {
            std::vector<A_UTF16Char> u16nm = ToUtf16(display_name);
            sp.LayerSuite9()->AEGP_SetLayerName(layer, u16nm.data());
        }

        sp.LayerSuite9()->AEGP_SetLayerOffset(layer, &offset);
        sp.LayerSuite9()->AEGP_SetLayerInPointAndDuration(
            layer, AEGP_LTimeMode_LayerTime, &layer_in_pt, &layer_dur);

        AEGP_LayerTransferMode tm{};
        tm.mode        = PF_Xfer_LIGHTEN;
        tm.flags       = static_cast<AEGP_TransferFlags>(0);
        tm.track_matte = AEGP_TrackMatte_NO_TRACK_MATTE;
        sp.LayerSuite9()->AEGP_SetLayerTransferMode(layer, &tm);

        {
            AEGP_StreamRefH op_stream = nullptr;
            sp.StreamSuite6()->AEGP_GetNewLayerStream(
                ctx.plugin_id, layer, AEGP_LayerStream_OPACITY, &op_stream);
            if (op_stream) {
                BakeThreeKeyframes(sp, op_stream,
                                   t0, 0.0,
                                   t1, chase.timing.opacity_peak,
                                   t2, 0.0);
                sp.StreamSuite6()->AEGP_DisposeStream(op_stream);
            }
        }

        if (ctx.demux_key != AEGP_InstalledEffectKey_NONE) {
            AEGP_EffectRefH effect = nullptr;
            if (!sp.EffectSuite5()->AEGP_ApplyEffect(
                    ctx.plugin_id, layer, ctx.demux_key, &effect) &&
                effect)
            {
                const uint32_t h32 = FNV1a32(display_name);
                const double hi = static_cast<double>((h32 >> 16) & 0xFFFF);
                const double lo = static_cast<double>(h32 & 0xFFFF);
                SetEffectFloatParam(sp, ctx.plugin_id, effect, 3, hi);
                SetEffectFloatParam(sp, ctx.plugin_id, effect, 4, lo);
                sp.EffectSuite5()->AEGP_DisposeEffect(effect);
            }
        }

        if (ctx.exposure_key != AEGP_InstalledEffectKey_NONE) {
            AEGP_EffectRefH expo = nullptr;
            if (!sp.EffectSuite5()->AEGP_ApplyEffect(
                    ctx.plugin_id, layer, ctx.exposure_key, &expo) &&
                expo)
            {
                AEGP_StreamRefH gamma = nullptr;
                FindEffectStreamByDisplayName(
                    sp, ctx.plugin_id, expo,
                    std::string("Gamma Correction"), &gamma);
                if (gamma) {
                    BakeThreeKeyframes(sp, gamma,
                                       t0, chase.timing.gamma_baseline,
                                       t1, chase.timing.gamma_peak,
                                       t2, chase.timing.gamma_baseline);
                    sp.StreamSuite6()->AEGP_DisposeStream(gamma);
                }
                sp.EffectSuite5()->AEGP_DisposeEffect(expo);
            }
        }
    };

    // Loop-mode placement: ONE time-locked footage layer per light
    // spanning the whole comp (offset 0 — the sequence plays its own
    // frames at the same comp time for every light, keeping the scene
    // animation in sync). The chase is expressed purely as wrapped
    // opacity/gamma keyframes whose envelope is periodic with the
    // loop, so the comp loops seamlessly with no from-black bookends.
    const double loopL   = static_cast<double>(total_frames);
    const double env_a   = attack_f;
    const double env_d   = duration_f;
    auto env_at = [loopL, env_a, env_d](double phase, double t) -> double {
        double local = std::fmod(t - phase, loopL);
        if (local < 0.0) local += loopL;
        if (local >= env_d) return 0.0;
        const double dd = env_d < 0.001 ? 0.001 : env_d;
        double pk = env_a / dd;
        if (pk < 0.001) pk = 0.001; else if (pk > 0.999) pk = 0.999;
        const double t01 = local / dd;
        return (t01 < pk) ? (t01 / pk) : ((1.0 - t01) / (1.0 - pk));
    };

    auto place_loop_layer = [&](const std::string& display_name,
                                double phase) {
        if (display_name.empty()) return;
        AEGP_LayerH layer = nullptr;
        if (sp.LayerSuite9()->AEGP_AddLayer(ctx.exr_footage, new_comp,
                                            &layer) || !layer)
            return;
        ++ctx.layers_added;

        // Name the timeline layer after the LIGHT (footage source
        // keeps the filename) so per-light layers are identifiable.
        {
            std::vector<A_UTF16Char> u16nm = ToUtf16(display_name);
            sp.LayerSuite9()->AEGP_SetLayerName(layer, u16nm.data());
        }

        // Time-locked: offset 0, span the whole comp. No SetLayerOffset
        // — the footage frame N lands on comp frame N for every light.
        const A_Time zero = FramesToATime(0.0, ctx.fps);
        sp.LayerSuite9()->AEGP_SetLayerOffset(layer, &zero);
        sp.LayerSuite9()->AEGP_SetLayerInPointAndDuration(
            layer, AEGP_LTimeMode_LayerTime, &zero, &duration);

        AEGP_LayerTransferMode tm{};
        tm.mode        = PF_Xfer_LIGHTEN;
        tm.flags       = static_cast<AEGP_TransferFlags>(0);
        tm.track_matte = AEGP_TrackMatte_NO_TRACK_MATTE;
        sp.LayerSuite9()->AEGP_SetLayerTransferMode(layer, &tm);

        // Sample the wrapped envelope at every piecewise-linear corner
        // (window start/peak/end, plus their ±loop copies that fall in
        // range) and the loop endpoints. Linear interp between these
        // reproduces the triangle exactly; env(0)==env(loop) keeps the
        // loop seamless.
        std::vector<double> ts;
        ts.push_back(0.0);
        ts.push_back(loopL);
        for (int k = -1; k <= 1; ++k) {
            for (double c : { 0.0, env_a, env_d }) {
                double tc = phase + c + static_cast<double>(k) * loopL;
                if (tc > 1e-4 && tc < loopL - 1e-4) ts.push_back(tc);
            }
        }
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end(),
            [](double a, double b){ return std::fabs(a - b) < 1e-4; }),
            ts.end());

        std::vector<std::pair<A_Time, double>> op_kf, gm_kf;
        op_kf.reserve(ts.size());
        gm_kf.reserve(ts.size());
        for (double tf : ts) {
            const double e = env_at(phase, tf);
            const A_Time at = FramesToATime(tf, ctx.fps);
            op_kf.emplace_back(at, chase.timing.opacity_peak * e);
            gm_kf.emplace_back(at, chase.timing.gamma_baseline +
                (chase.timing.gamma_peak - chase.timing.gamma_baseline) * e);
        }

        {
            AEGP_StreamRefH op_stream = nullptr;
            sp.StreamSuite6()->AEGP_GetNewLayerStream(
                ctx.plugin_id, layer, AEGP_LayerStream_OPACITY, &op_stream);
            if (op_stream) {
                BakeKeyframes(sp, op_stream, op_kf);
                sp.StreamSuite6()->AEGP_DisposeStream(op_stream);
            }
        }

        if (ctx.demux_key != AEGP_InstalledEffectKey_NONE) {
            AEGP_EffectRefH effect = nullptr;
            if (!sp.EffectSuite5()->AEGP_ApplyEffect(
                    ctx.plugin_id, layer, ctx.demux_key, &effect) &&
                effect)
            {
                const uint32_t h32 = FNV1a32(display_name);
                const double hi = static_cast<double>((h32 >> 16) & 0xFFFF);
                const double lo = static_cast<double>(h32 & 0xFFFF);
                SetEffectFloatParam(sp, ctx.plugin_id, effect, 3, hi);
                SetEffectFloatParam(sp, ctx.plugin_id, effect, 4, lo);
                sp.EffectSuite5()->AEGP_DisposeEffect(effect);
            }
        }

        if (ctx.exposure_key != AEGP_InstalledEffectKey_NONE) {
            AEGP_EffectRefH expo = nullptr;
            if (!sp.EffectSuite5()->AEGP_ApplyEffect(
                    ctx.plugin_id, layer, ctx.exposure_key, &expo) &&
                expo)
            {
                AEGP_StreamRefH gamma = nullptr;
                FindEffectStreamByDisplayName(
                    sp, ctx.plugin_id, expo,
                    std::string("Gamma Correction"), &gamma);
                if (gamma) {
                    BakeKeyframes(sp, gamma, gm_kf);
                    sp.StreamSuite6()->AEGP_DisposeStream(gamma);
                }
                sp.EffectSuite5()->AEGP_DisposeEffect(expo);
            }
        }
    };

    auto name_of = [&](const LayerRef& ref) -> std::string {
        std::lock_guard<std::mutex> lk(ctx.state->mu);
        if (const LayerInfo* L = FindLayerByRef(*ctx.state, ref))
            return L->display_name;
        return {};
    };

    if (loop_mode) {
        // Animation source: seamless loop of exactly the source
        // duration. Footage time-locked; the chase IS the wrapped
        // envelope. Scatter picks phases via the stratified
        // generator; templated chases space stages evenly over the
        // loop (step = loop / stageCount, fractional frames fine —
        // the loop divides the source exactly, which is what matters).
        if (chase.random_scatter) {
            Chase local = chase;
            {
                std::lock_guard<std::mutex> lk(ctx.state->mu);
                RegenerateScatter(local, *ctx.state,
                                  static_cast<float>(fps_d));
            }
            for (const ScatterHit& hit : local.scatter) {
                const std::string nm = name_of(hit.ref);
                if (!nm.empty())
                    place_loop_layer(nm, hit.start_frame);
            }
        } else {
            const size_t n = chase.stages.size();
            for (size_t si = 0; si < n; ++si) {
                const double phase = (n == 0) ? 0.0
                    : static_cast<double>(si) * (loopL /
                          static_cast<double>(n));
                for (const LayerRef& ref : chase.stages[si].members)
                    place_loop_layer(name_of(ref), phase);
            }
        }
    } else if (chase.random_scatter) {
        // Regenerate the scatter HERE rather than trusting
        // chase.scatter — that list is filled by the UI only while
        // the scatter tab is drawn and is not persisted, so a
        // session-loaded chase or a Build-all would otherwise emit a
        // comp with zero light layers (all black). Deterministic from
        // the chase's seed/density/loop + comp fps.
        Chase local = chase;
        {
            std::lock_guard<std::mutex> lk(ctx.state->mu);
            RegenerateScatter(local, *ctx.state,
                              static_cast<float>(fps_d));
        }
        // Every hit is a layer at its scattered offset. A hit whose
        // envelope tail crosses the loop end gets a second copy at
        // offset-loop so the tail re-enters at the comp start — the
        // comp then loops seamlessly at `total_frames` (no black
        // bookends: it's a continuous loop, not a one-shot sweep).
        const double loop_f = static_cast<double>(total_frames);
        for (const ScatterHit& hit : local.scatter) {
            const std::string nm = name_of(hit.ref);
            if (nm.empty()) continue;
            place_layer(nm, offset_at(hit.start_frame));
            if (hit.start_frame + duration_f > loop_f) {
                place_layer(nm, offset_at(hit.start_frame - loop_f));
            }
        }
    } else {
        for (size_t si = 0; si < chase.stages.size(); ++si) {
            const double stage_start =
                static_cast<double>(si) * chase.timing.step_duration;
            const A_Time stage_offset = offset_at(stage_start);
            for (const LayerRef& ref : chase.stages[si].members) {
                place_layer(name_of(ref), stage_offset);
            }
        }
    }

    return A_Err_NONE;
}

BuildResult DoBuild(PanelState* state, int chase_index, bool build_all)
{
    BuildResult result;
    if (!state || !state->pica_basicP) {
        result.message = "Build skipped — no AEGP context.";
        return result;
    }
    AEGP_SuiteHandler sp(reinterpret_cast<SPBasicSuite*>(state->pica_basicP));
    BuildContext ctx;
    ctx.state = state;
    ctx.plugin_id = static_cast<AEGP_PluginID>(state->aegp_plugin_id);

    // Pull active project. AE typically always has one open; the
    // SDK samples all use index 0. If this fails, also probe
    // ItemSuite to see if AEGP works at all from this context.
    {
        A_long num_proj = 0;
        sp.ProjSuite6()->AEGP_GetNumProjects(&num_proj);
        A_Err err = sp.ProjSuite6()->AEGP_GetProjectByIndex(0, &ctx.proj);
        if (err || !ctx.proj) {
            if (num_proj >= 1) {
                ctx.proj = nullptr;
                err = sp.ProjSuite6()->AEGP_GetProjectByIndex(1, &ctx.proj);
            }
        }
        if (err || !ctx.proj) {
            // Secondary probe: does any AEGP suite work right now?
            AEGP_ItemH active = nullptr;
            A_Err ierr = sp.ItemSuite9()->AEGP_GetActiveItem(&active);
            AEGP_ItemH root = nullptr;
            // GetProjectRootFolder needs a project handle we don't
            // have, so probe with a NULL handle and see what AE says.
            A_Err rerr = ierr;
            (void)rerr; (void)root;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "No active AE project (GetProjectByIndex err=%ld, num_proj=%ld; "
                "GetActiveItem err=%ld, item=%p; pica=%p, plugin_id=%d).",
                static_cast<long>(err), static_cast<long>(num_proj),
                static_cast<long>(ierr), (void*)active,
                state->pica_basicP, state->aegp_plugin_id);
            result.message = buf;
            return result;
        }
    }

    // Source dimensions + path (from the active source).
    std::string src_path, src_scan_path;
    bool src_is_anim = false;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const Source* src = ActiveSource(*state);
        if (!src || src->path.empty()) {
            result.message = "No active source loaded — nothing to build.";
            return result;
        }
        src_path = src->path;
        src_scan_path = src->scan_path;
        ctx.src_w = src->image_width;
        ctx.src_h = src->image_height;
        // Loop-mode when the source is an animation/sequence; the
        // loop is EXACTLY the source duration so the chase stays
        // phase-locked to the scene animation.
        if (src->animation && src->frame_count > 1)
            ctx.loop_frames = src->frame_count;
        src_is_anim = (ctx.loop_frames > 0);
    }
    if (ctx.src_w < 1 || ctx.src_h < 1) {
        result.message = "Active source has zero dimensions; "
                          "wait for scan to complete and retry.";
        return result;
    }

    // The single project FPS is authoritative for every chase
    // (preview + build). It's auto-seeded from the active comp but
    // the user can pin it in the Sources tab — that's what fixes the
    // "built at 24 while the project is 30" bug.
    ctx.fps = RatioFromFps(static_cast<A_FpLong>(state->project_fps.load()));

    // Wrap everything in a single undo group so the user can Ctrl+Z
    // the whole build out of AE in one stroke.
    sp.UtilitySuite6()->AEGP_StartUndoGroup("ChaseMaker — Build chase");

    // Target folder + ChaseMaker_vNN folder.
    if (FindTargetFolder(sp, ctx.proj, &ctx.target_folder) || !ctx.target_folder) {
        sp.UtilitySuite6()->AEGP_EndUndoGroup();
        result.message = "Could not resolve target folder.";
        return result;
    }
    ctx.chasemaker_folder_name = "ChaseMaker";
    {
        // Reuse a single project-root "ChaseMaker" folder across
        // builds instead of spawning ChaseMaker_vNN on every export.
        // Comps inside are still name-versioned (_vNN on collision),
        // so nothing is overwritten — this just stops the folder
        // explosion the user hit while iterating.
        AEGP_ItemH existing = nullptr;
        FindChildByName(sp, ctx.proj, ctx.target_folder,
                        ctx.chasemaker_folder_name, AEGP_ItemType_FOLDER,
                        ctx.plugin_id, &existing);
        if (existing) {
            ctx.chasemaker_folder = existing;
        } else {
            std::vector<A_UTF16Char> u16 =
                ToUtf16(ctx.chasemaker_folder_name);
            A_Err e = sp.ItemSuite9()->AEGP_CreateNewFolder(
                u16.data(), ctx.target_folder, &ctx.chasemaker_folder);
            if (e || !ctx.chasemaker_folder) {
                sp.UtilitySuite6()->AEGP_EndUndoGroup();
                result.message = "Could not create ChaseMaker folder (err " +
                                  std::to_string(e) + ").";
                return result;
            }
        }
    }

    // Footage.
    if (EnsureFootage(sp, ctx.proj, ctx.plugin_id, src_path,
                      src_scan_path, src_is_anim,
                      ctx.chasemaker_folder, &ctx.exr_footage) ||
        !ctx.exr_footage)
    {
        sp.UtilitySuite6()->AEGP_EndUndoGroup();
        result.message = "Could not find/import source footage: " + src_path;
        return result;
    }

    // Loop-mode comp length is the source's own frame count
    // (ctx.loop_frames, set from src->frame_count) laid out at the
    // user-authoritative project FPS — so a 60-frame source at 30 fps
    // is exactly 2:00, no footage-fps guessing.

    // Shared black-solid backdrop. AE's NewSolidFootage takes an
    // A_char* name (ASCII), not UTF-16. Sized to the source so it
    // covers the entire comp frame. Reuse the existing solid in the
    // (now-shared) ChaseMaker folder if it's the right size, so
    // repeated builds don't pile up identical backdrops; only make a
    // new one if missing or the source dimensions changed.
    {
        AEGP_ItemH existing_bg = nullptr;
        FindChildByName(sp, ctx.proj, ctx.chasemaker_folder,
                        "ChaseMaker BG (black)", AEGP_ItemType_FOOTAGE,
                        ctx.plugin_id, &existing_bg);
        if (existing_bg) {
            A_long bw = 0, bh = 0;
            sp.ItemSuite9()->AEGP_GetItemDimensions(existing_bg, &bw, &bh);
            if (bw == ctx.src_w && bh == ctx.src_h)
                ctx.black_solid = existing_bg;
        }
        if (!ctx.black_solid) {
            AEGP_ColorVal black{};
            black.alphaF = 1.0;
            black.redF = black.greenF = black.blueF = 0.0;
            AEGP_FootageH solid_fh = nullptr;
            if (!sp.FootageSuite5()->AEGP_NewSolidFootage(
                    "ChaseMaker BG (black)",
                    ctx.src_w, ctx.src_h, &black, &solid_fh) && solid_fh)
            {
                sp.FootageSuite5()->AEGP_AddFootageToProject(
                    solid_fh, ctx.chasemaker_folder, &ctx.black_solid);
            }
        }
    }

    // EXRDemux effect — required for layer-name-driven render.
    FindInstalledEffectKey(sp, "tdcarney EXRDemux", &ctx.demux_key);
    if (ctx.demux_key == AEGP_InstalledEffectKey_NONE) {
        result.exrdemux_missing = true;
        result.message = "EXRDemux ('tdcarney EXRDemux') is not installed "
                         "— light layers were added WITHOUT hash-driven "
                         "selection and will render wrong. Save your "
                         "session, install EXRDemux, restart AE, then "
                         "Load session and Build again.";
    }
    // Built-in Exposure effect — used to bake the Gamma Correction
    // envelope. Match name is stable across AE versions.
    FindInstalledEffectKey(sp, "ADBE Exposure2", &ctx.exposure_key);
    if (ctx.exposure_key == AEGP_InstalledEffectKey_NONE) {
        if (result.message.empty()) {
            result.message = "Note: ADBE Exposure2 not found; layers "
                             "built without Gamma envelope.";
        } else {
            result.message += " (Exposure effect also missing.)";
        }
    }

    // Build all chases or just the one.
    auto build_idx = [&](int i) -> A_Err {
        if (i < 0 || i >= (int)state->chases.size()) return A_Err_NONE;
        Chase c;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (i >= (int)state->chases.size()) return A_Err_NONE;
            c = state->chases[i];
        }
        return BuildOneChase(sp, c, ctx);
    };

    if (build_all) {
        std::vector<int> indices;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            for (int i = 0; i < (int)state->chases.size(); ++i) indices.push_back(i);
        }
        for (int i : indices) build_idx(i);
    } else {
        build_idx(chase_index);
    }

    sp.UtilitySuite6()->AEGP_EndUndoGroup();

    result.success = (ctx.comps_created > 0);
    result.comps_created = ctx.comps_created;
    result.layers_added  = ctx.layers_added;
    if (result.message.empty()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "Built %d comp(s), %d layer(s) into %s.",
            ctx.comps_created, ctx.layers_added,
            ctx.chasemaker_folder_name.c_str());
        result.message = buf;
    } else if (result.success) {
        result.message += " — and built " +
            std::to_string(ctx.comps_created) + " comp(s).";
    }
    return result;
}

} // namespace

BuildResult BuildChase(PanelState* state, int chase_index)
{
    BuildResult r = DoBuild(state, chase_index, /*build_all=*/false);
    if (state) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_build_status = r.message;
        state->last_build_exrdemux_missing = r.exrdemux_missing;
    }
    return r;
}

BuildResult BuildAllChases(PanelState* state)
{
    BuildResult r = DoBuild(state, /*chase_index=*/-1, /*build_all=*/true);
    if (state) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_build_status = r.message;
        state->last_build_exrdemux_missing = r.exrdemux_missing;
    }
    return r;
}

void RefreshProjectFps(PanelState* state)
{
    if (!state || !state->pica_basicP) return;
    // Once the user has set the FPS in the Sources tab, their value
    // is authoritative — stop auto-tracking the active comp.
    if (state->project_fps_user.load()) return;
    AEGP_SuiteHandler sp(reinterpret_cast<SPBasicSuite*>(state->pica_basicP));
    A_Ratio r = GetActiveCompFramerate(sp);
    if (r.num > 0 && r.den > 0) {
        state->project_fps.store(
            static_cast<float>(r.num) / static_cast<float>(r.den));
    }
}

} // namespace ae_build
