#include "ae_build.h"

#include "panel_state.h"
#include "hash.h"
#include "exr_scan.h"          // AnalyzeFramePixels (shared metric/thumb math)

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
#include <unordered_map>
#include <unordered_set>
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
//
// CRITICAL: we scale value AND scale by `kSub` (1000) to preserve
// subframe precision. Without this, fps={30,1} (the default 30 fps
// path) loses everything past the decimal in `frames` — value =
// (frames*1)+0.5 truncates so 59.53 and 60.0 both collapse to
// A_Time 60/30 = 2.0s. The wrapped-triangle envelope's subframe
// corners (start/peak/end of each cycle hit) ALL collapse to the
// same integer frame, destroying the chase shape and producing a
// visible flash at the loop boundary when cycles > 1 (or any
// template whose stage step is fractional, which is most of them).
// The 1000x multiplier gives 1ms-equivalent resolution at 30 fps
// and only inflates `value` by 1000 — still well inside A_long
// range for any realistic comp length.
A_Time FramesToATime(double frames, A_Ratio fps)
{
    A_Time t{};
    if (fps.num <= 0) {
        t.value = 0;
        t.scale = 1;
        return t;
    }
    constexpr A_long kSub = 1000;
    t.value = static_cast<A_long>(frames * fps.den * kSub + 0.5);
    t.scale = static_cast<A_u_long>(fps.num) * kSub;
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
    AEGP_ItemH                exr_footage = nullptr;        // active source (default)
    // Per-source footage: a chase can pull lights from multiple
    // sources (e.g. one multilayer EXR + several single-layer
    // extras), and each light's layer must come from ITS source's
    // footage. Looked up by ref.source_id in place_layer /
    // place_loop_layer; falls back to `exr_footage` if missing.
    std::unordered_map<uint32_t, AEGP_ItemH> footages;
    // Source ids that are self-contained movie clips (one light, no
    // layers). Their footage is imported as a single file (not an
    // all_in_folder sequence) and the build applies NO EXRDemux to
    // their layers — there's nothing to demux. Consulted by
    // place_layer / place_loop_layer to skip the demux+hash block.
    std::unordered_set<uint32_t> movie_source_ids;
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
    // Opacity peak + floor (the trough the envelope bottoms out at).
    // floor clamped into [0, peak]. floor == 0 reproduces the legacy
    // full-off trough.
    const double op_peak  = chase.timing.opacity_peak;
    const double op_floor = std::max(0.0,
        std::min<double>(chase.timing.opacity_floor, op_peak));
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
                           const std::string& render_name,
                           uint32_t source_id,
                           const A_Time& offset) {
        if (display_name.empty()) return;
        // Pick the FOOTAGE of the light's source (multi-source).
        auto fit = ctx.footages.find(source_id);
        AEGP_ItemH foot = (fit != ctx.footages.end()) ? fit->second
                                                       : ctx.exr_footage;
        if (!foot) return;
        AEGP_LayerH layer = nullptr;
        if (sp.LayerSuite9()->AEGP_AddLayer(foot, new_comp,
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
                                   t0, op_floor,
                                   t1, op_peak,
                                   t2, op_floor);
                sp.StreamSuite6()->AEGP_DisposeStream(op_stream);
            }
        }

        if (ctx.demux_key != AEGP_InstalledEffectKey_NONE &&
            !ctx.movie_source_ids.count(source_id)) {
            AEGP_EffectRefH effect = nullptr;
            if (!sp.EffectSuite5()->AEGP_ApplyEffect(
                    ctx.plugin_id, layer, ctx.demux_key, &effect) &&
                effect)
            {
                // Hash the EXR-INTERNAL name — display_name may have
                // been renamed for the timeline label, but the file
                // still contains the original layer name and
                // EXRDemux hashes that at render.
                const std::string& demux_name =
                    render_name.empty() ? display_name : render_name;
                const uint32_t h32 = FNV1a32(demux_name);
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
    // Cycles tile the chase pattern N times INSIDE the loop. The
    // envelope wraps on `period = loopL / cycles` so a single phase
    // produces `cycles` triangle hits per comp automatically.
    // Scatter loop-mode keeps period = loopL (its hits are already
    // distributed by the stratified generator; cycles is a templated-
    // chase concept).
    const int    cycles  = chase.random_scatter
                               ? 1
                               : (chase.loop_cycles < 1 ? 1 : chase.loop_cycles);
    const double period  = (cycles > 1) ? (loopL / static_cast<double>(cycles))
                                        : loopL;
    const double env_hold = chase.timing.hold < 0.f
        ? 0.0 : static_cast<double>(chase.timing.hold);
    const double env_total = env_d + env_hold;
    auto env_at = [period, env_a, env_d, env_hold, env_total]
                  (double phase, double t) -> double {
        double local = std::fmod(t - phase, period);
        if (local < 0.0) local += period;
        if (local >= env_total) return 0.0;
        const double att = (env_a < 0.001)        ? 0.001
                         : (env_a > env_d - 0.001) ? (env_d - 0.001)
                                                   : env_a;
        if (local < att)                return local / att;          // rising
        if (local < att + env_hold)     return 1.0;                  // holding
        const double fall_len = (env_d - att) < 0.001 ? 0.001
                                                      : (env_d - att);
        return (env_total - local) / fall_len;                       // falling
    };

    auto place_loop_layer = [&](const std::string& display_name,
                                const std::string& render_name,
                                uint32_t source_id,
                                double phase) {
        if (display_name.empty()) return;
        auto fit = ctx.footages.find(source_id);
        AEGP_ItemH foot = (fit != ctx.footages.end()) ? fit->second
                                                       : ctx.exr_footage;
        if (!foot) return;
        AEGP_LayerH layer = nullptr;
        if (sp.LayerSuite9()->AEGP_AddLayer(foot, new_comp,
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

        // Sample the wrapped envelope at every piecewise-linear
        // corner — window start/peak/end-of-hold/end-of-fall
        // shifted by k*period for each cycle in the loop, plus ±1
        // neighbor for wrap. Linear interp between these reproduces
        // the triangle (with optional hold plateau) exactly.
        //
        // Seamless loop comes from env_at's periodicity: cycles *
        // period == loopL, so env(loopL) == env(0). AE renders frame
        // loopL-1 with the natural envelope value and frame 0 of the
        // next iteration with env(0); those values are 1 continuous-
        // time frame apart, exactly like any other transition. We do
        // NOT explicitly pin frame loopL-1 to env(0) — that produces
        // a 2-frame freeze on every loop iteration (the
        // "duplicate-frame stutter" bug).
        std::vector<double> ts;
        ts.push_back(0.0);
        ts.push_back(loopL);
        for (int k = -1; k <= cycles; ++k) {
            // Corners of the envelope shape: start (env=0), peak
            // attack (env=1), end of hold (env=1, only meaningful
            // when hold > 0), and end of fall (env=0). Including
            // both peak corners is what makes the held-at-1
            // plateau reproduce exactly with linear interpolation.
            for (double c : { 0.0, env_a, env_a + env_hold, env_total }) {
                double tc = phase + c + static_cast<double>(k) * period;
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
            op_kf.emplace_back(at, op_floor + (op_peak - op_floor) * e);
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

        if (ctx.demux_key != AEGP_InstalledEffectKey_NONE &&
            !ctx.movie_source_ids.count(source_id)) {
            AEGP_EffectRefH effect = nullptr;
            if (!sp.EffectSuite5()->AEGP_ApplyEffect(
                    ctx.plugin_id, layer, ctx.demux_key, &effect) &&
                effect)
            {
                // Hash the EXR-INTERNAL name — display_name may have
                // been renamed for the timeline label, but the file
                // still contains the original layer name and
                // EXRDemux hashes that at render.
                const std::string& demux_name =
                    render_name.empty() ? display_name : render_name;
                const uint32_t h32 = FNV1a32(demux_name);
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
    // EXR-internal name for the EXRDemux hash — display_name may
    // have been renamed (e.g. single-light source → filename base),
    // but EXRDemux at render hashes the file's actual layer name.
    // Falls back to display_name if exr_layer_name is empty (older
    // session). Returns (display_name, render_name).
    auto names_of = [&](const LayerRef& ref)
        -> std::pair<std::string, std::string>
    {
        std::lock_guard<std::mutex> lk(ctx.state->mu);
        if (const LayerInfo* L = FindLayerByRef(*ctx.state, ref)) {
            return { L->display_name,
                     L->exr_layer_name.empty() ? L->display_name
                                               : L->exr_layer_name };
        }
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
                auto nms = names_of(hit.ref);
                if (!nms.first.empty())
                    place_loop_layer(nms.first, nms.second,
                                     hit.ref.source_id, hit.start_frame);
            }
        } else {
            const size_t n = chase.stages.size();
            // Master offset shifts every stage's phase. Stages
            // spread across [0, period - env_d - 1] within each
            // cycle so no hit straddles the cycle boundary — env
            // naturally passes through zero at every cycle edge,
            // including frame loop_frames - 1, giving a clean loop
            // wrap. Offset > 0 can push some stages back into
            // straddle; the explicit loop_frames - 1 keyframe in
            // place_loop_layer catches that residual case.
            int off_i = chase.loop_offset;
            const int per_i = static_cast<int>(period + 0.5);
            if (per_i > 0) {
                off_i %= per_i;
                if (off_i < 0) off_i += per_i;
            } else {
                off_i = 0;
            }
            const double off_d = static_cast<double>(off_i);
            // Stages evenly spaced across the full cycle period.
            // step = period / n = loop_frames / (n * cycles): the
            // gap between consecutive hits anywhere in the loop.
            // env_d <= step → no tail; env_d > step → overlap.
            const double step = (n == 0) ? 0.0
                : period / static_cast<double>(n);
            for (size_t si = 0; si < n; ++si) {
                const double phase = static_cast<double>(si) * step
                                     + off_d;
                for (const LayerRef& ref : chase.stages[si].members) {
                    auto nms = names_of(ref);
                    place_loop_layer(nms.first, nms.second,
                                     ref.source_id, phase);
                }
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
            auto nms = names_of(hit.ref);
            if (nms.first.empty()) continue;
            place_layer(nms.first, nms.second, hit.ref.source_id,
                        offset_at(hit.start_frame));
            if (hit.start_frame + duration_f > loop_f) {
                place_layer(nms.first, nms.second, hit.ref.source_id,
                            offset_at(hit.start_frame - loop_f));
            }
        }
    } else {
        for (size_t si = 0; si < chase.stages.size(); ++si) {
            const double stage_start =
                static_cast<double>(si) * chase.timing.step_duration;
            const A_Time stage_offset = offset_at(stage_start);
            for (const LayerRef& ref : chase.stages[si].members) {
                auto nms = names_of(ref);
                place_layer(nms.first, nms.second,
                            ref.source_id, stage_offset);
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
    // Whether the ACTIVE source is an on-disk frame sequence (EXR/PNG
    // sequence) — that's what needs all_in_folder import. A movie or a
    // single still is NOT a sequence even though it may be loop-mode.
    bool active_is_seq = false;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const Source* src = ActiveSource(*state);
        if (!src || src->path.empty()) {
            result.message = "No active source loaded — nothing to build.";
            return result;
        }
        src_path = src->path;
        src_scan_path = src->scan_path;
        active_is_seq =
            (src->animation && src->frame_count > 1 && !src->is_movie);
        ctx.src_w = src->image_width;
        ctx.src_h = src->image_height;
        // Loop-mode is a SESSION property: if ANY loaded source is an
        // animation/sequence, the whole build loops over its duration
        // (so still lights — e.g. lone PNGs — ride the movies' loop and
        // stay phase-locked). Not tied to which source happens to be
        // active. The active source still supplies the comp dimensions.
        if (const Source* anim = SessionAnimationSource(*state))
            ctx.loop_frames = anim->frame_count;
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

    // Footage. Only on-disk frame sequences (EXR/PNG sequences) use the
    // all_in_folder import; a movie or single still imports as one file
    // even though the session is loop-mode.
    if (EnsureFootage(sp, ctx.proj, ctx.plugin_id, src_path,
                      src_scan_path, active_is_seq,
                      ctx.chasemaker_folder, &ctx.exr_footage) ||
        !ctx.exr_footage)
    {
        sp.UtilitySuite6()->AEGP_EndUndoGroup();
        result.message = "Could not find/import source footage: " + src_path;
        return result;
    }

    // Multi-source: ensure footage for EVERY loaded source so a
    // chase that pulls lights from more than one source (e.g. one
    // multilayer EXR + several single-layer extras) places each
    // light using its OWN source's footage. Snapshot all sources
    // under lock, then ensure-or-find each. Failure on a non-active
    // source is not fatal — its lights just won't be placed.
    struct SrcSnap {
        uint32_t id; std::string path; std::string scan; bool anim;
    };
    std::vector<SrcSnap> srcs;
    uint32_t active_sid = 0;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        for (const Source& s : state->sources) {
            if (s.path.empty()) continue;
            // Movies import as a single file (not all_in_folder), so
            // exclude them from the sequence-import flag even though
            // they're loop-mode. Also record movie ids so the demux
            // step is skipped for their layers.
            srcs.push_back({ s.source_id, s.path, s.scan_path,
                              s.animation && s.frame_count > 1 &&
                              !s.is_movie });
            if (s.is_movie) ctx.movie_source_ids.insert(s.source_id);
        }
        if (const Source* a = ActiveSource(*state)) active_sid = a->source_id;
    }
    if (active_sid != 0) ctx.footages[active_sid] = ctx.exr_footage;
    for (const SrcSnap& s : srcs) {
        if (s.id == active_sid) continue;   // already imported above
        AEGP_ItemH foot = nullptr;
        if (!EnsureFootage(sp, ctx.proj, ctx.plugin_id, s.path,
                           s.scan, s.anim, ctx.chasemaker_folder, &foot)
            && foot)
        {
            ctx.footages[s.id] = foot;
        }
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

// ===== Movie-source analysis (AE renders one frame) ===================
//
// A movie source (.mov/.mp4/.mxf) is one self-contained light. The
// OpenEXR scanner can't read it, so we get pixels the only way that
// works reliably on both platforms (AE is the only dependable ProRes
// decoder on Windows): take the clip's footage item, ask AE to render a
// single downsampled 32-bit frame, and copy the pixels out. We PREFER
// the user's existing project footage for this path (so its conform-fps
// / Interpret Footage is honored — and matches what the build uses);
// only if the clip isn't already in the project do we import a
// throwaway temp, which is then deleted. Duration of that item gives
// the loop frame count at the project fps; the centroid metrics +
// thumbnail are computed by exr_scan::AnalyzeFramePixels — the exact
// same math the EXR scan uses. Runs on the main thread from the idle
// hook (AEGP render + item calls require that context).
void AnalyzeOneMovie(AEGP_SuiteHandler& sp, AEGP_ProjectH proj,
                     AEGP_PluginID plugin_id, PanelState* state,
                     uint32_t source_id)
{
    std::string path;
    int   thumb_w_req = 384;
    float fps = 30.f;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const Source* src = FindSourceById(*state, source_id);
        if (!src) return;
        path         = src->path;
        thumb_w_req  = state->thumb_max_width;
        fps          = state->project_fps.load();
    }
    if (path.empty()) return;
    if (fps < 1.f) fps = 30.f;

    AEGP_ItemH target_folder = nullptr;
    FindTargetFolder(sp, proj, &target_folder);

    sp.UtilitySuite6()->AEGP_StartUndoGroup("ChaseMaker — Analyze clip");

    // Prefer the user's EXISTING project footage for this path, so we
    // honor any Interpret Footage (conform fps) they've applied — AE
    // reports the CONFORMED duration for it, which is what the build
    // uses too (EnsureFootage reuses the same item). Only when the clip
    // isn't already in the project do we import a throwaway temp (native
    // interpretation), which is deleted after. This is the fix for the
    // "comp too long" bug: analyzing a fresh native import read the
    // un-conformed length while the build used the conformed footage.
    AEGP_ItemH item = nullptr;
    bool imported_temp = false;
    FindFootageByPath(sp, proj, plugin_id, path, path, &item);
    if (!item) {
        AEGP_FootageH footH = nullptr;
        std::vector<A_UTF16Char> u16 = ToUtf16(path);
        A_Err err = sp.FootageSuite5()->AEGP_NewFootage(
            plugin_id, u16.data(),
            nullptr,   // no layer key — import merged
            nullptr,   // no sequence options — a movie is one file
            AEGP_InterpretationStyle_NO_DIALOG_GUESS,
            nullptr, &footH);
        if (!err && footH) {
            sp.FootageSuite5()->AEGP_AddFootageToProject(
                footH, target_folder, &item);
        }
        imported_temp = (item != nullptr);
    }
    if (!item) {
        sp.UtilitySuite6()->AEGP_EndUndoGroup();
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Clip analysis failed (could not import).";
        return;
    }

    // Read interpretation for transparency: native (file) fps vs the
    // conform fps the user may have set. Surfaced in the status so an
    // fps mismatch is visible rather than silently inflating the loop.
    double native_fps = 0.0, conform_fps = 0.0;
    {
        AEGP_FootageInterp interp{};
        if (!sp.FootageSuite5()->AEGP_GetFootageInterpretation(
                item, FALSE, &interp))
        {
            native_fps  = interp.native_fpsF;
            conform_fps = interp.conform_fpsF;
        }
    }

    // Dimensions + duration. Read the item's STILL flag to decide
    // still-vs-animation straight from the AE item (the answer to
    // "checkbox or read from the item"): a still image is ONE light that
    // doesn't loop on its own; a multi-frame item (movie / image
    // sequence) is an animation whose frame count drives loop mode.
    A_long iw = 0, ih = 0;
    sp.ItemSuite9()->AEGP_GetItemDimensions(item, &iw, &ih);
    AEGP_ItemFlags flags = 0;
    sp.ItemSuite9()->AEGP_GetItemFlags(item, &flags);
    const bool is_still = (flags & AEGP_ItemFlag_STILL) != 0;
    A_Time dur{0, 1};
    sp.ItemSuite9()->AEGP_GetItemDuration(item, &dur);
    const double dur_sec =
        (dur.scale != 0) ? static_cast<double>(dur.value) /
                           static_cast<double>(dur.scale)
                         : 0.0;
    // Still -> exactly one frame (no own loop). Otherwise the conformed
    // duration gives the clip's real frame count at the project fps.
    int frame_count = is_still ? 1
        : static_cast<int>(std::lround(dur_sec * static_cast<double>(fps)));
    if (frame_count < 1) frame_count = 1;
    const bool is_animation = (frame_count > 1) && !is_still;

    // Render one representative frame (~1/3 in) at 32-bit float,
    // downsampled to roughly the thumbnail width.
    std::vector<float> r, g, b;
    int  rw = 0, rh = 0;
    bool got_pixels = false;
    {
        AEGP_RenderOptionsH ro = nullptr;
        if (!sp.RenderOptionsSuite3()->AEGP_NewFromItem(plugin_id, item, &ro)
            && ro)
        {
            sp.RenderOptionsSuite3()->AEGP_SetWorldType(ro, AEGP_WorldType_32);
            // Still: render frame 0 (every frame is identical). Movie:
            // a representative frame ~1/3 in (a light that fades shows
            // mid-life rather than black at the head).
            A_Time t;
            t.scale = (dur.scale != 0) ? dur.scale : 1;
            t.value = is_still ? 0
                : static_cast<A_long>(static_cast<double>(dur.value) / 3.0);
            sp.RenderOptionsSuite3()->AEGP_SetTime(ro, t);
            short ds = 1;
            if (iw > 0 && thumb_w_req > 0) {
                long f = std::lround(static_cast<double>(iw) /
                                     static_cast<double>(thumb_w_req));
                ds = static_cast<short>(f < 1 ? 1 : f);
            }
            sp.RenderOptionsSuite3()->AEGP_SetDownsampleFactor(ro, ds, ds);

            AEGP_FrameReceiptH receipt = nullptr;
            if (!sp.RenderSuite5()->AEGP_RenderAndCheckoutFrame(
                    ro, nullptr, nullptr, &receipt) && receipt)
            {
                AEGP_WorldH world = nullptr;
                if (!sp.RenderSuite5()->AEGP_GetReceiptWorld(receipt, &world)
                    && world)
                {
                    AEGP_WorldType wt = AEGP_WorldType_NONE;
                    sp.WorldSuite3()->AEGP_GetType(world, &wt);
                    A_long ww = 0, wh = 0;
                    sp.WorldSuite3()->AEGP_GetSize(world, &ww, &wh);
                    A_u_long rb = 0;
                    sp.WorldSuite3()->AEGP_GetRowBytes(world, &rb);
                    PF_PixelFloat* base = nullptr;
                    if (wt == AEGP_WorldType_32 && ww > 0 && wh > 0 &&
                        !sp.WorldSuite3()->AEGP_GetBaseAddr32(world, &base) &&
                        base)
                    {
                        rw = ww; rh = wh;
                        const size_t n = static_cast<size_t>(ww) * wh;
                        r.resize(n); g.resize(n); b.resize(n);
                        for (int y = 0; y < wh; ++y) {
                            const PF_PixelFloat* rowp =
                                reinterpret_cast<const PF_PixelFloat*>(
                                    reinterpret_cast<const char*>(base) +
                                    static_cast<size_t>(y) * rb);
                            for (int x = 0; x < ww; ++x) {
                                const size_t i =
                                    static_cast<size_t>(y) * ww + x;
                                r[i] = rowp[x].red;
                                g[i] = rowp[x].green;
                                b[i] = rowp[x].blue;
                            }
                        }
                        got_pixels = true;
                    }
                }
                sp.RenderSuite5()->AEGP_CheckinFrame(receipt);
            }
            sp.RenderOptionsSuite3()->AEGP_Dispose(ro);
        }
    }

    // Remove ONLY a temp we imported — never the user's own footage.
    if (imported_temp) sp.ItemSuite9()->AEGP_DeleteItem(item);
    sp.UtilitySuite6()->AEGP_EndUndoGroup();

    // Publish onto the source's single LayerInfo.
    {
        std::lock_guard<std::mutex> lk(state->mu);
        Source* src = FindSourceById(*state, source_id);
        if (!src) return;
        if (iw > 0) src->image_width  = iw;
        if (ih > 0) src->image_height = ih;
        src->frame_count = frame_count;
        src->animation   = is_animation;
        if (!src->layers.empty() && got_pixels && rw > 1 && rh > 1) {
            exr_scan::AnalyzeFramePixels(r, g, b, rw, rh, thumb_w_req,
                                         src->layers[0]);
        }
        const double eff_fps = conform_fps > 0.01 ? conform_fps : native_fps;
        char buf[256];
        if (is_still) {
            std::snprintf(buf, sizeof(buf),
                "Still analyzed: %s (%ldx%ld, single light).",
                src->layers.empty() ? "?" : src->layers[0].display_name.c_str(),
                static_cast<long>(src->image_width),
                static_cast<long>(src->image_height));
        } else {
            std::snprintf(buf, sizeof(buf),
                "Clip analyzed: %s (%ldx%ld, %d frame%s @ %.2f fps; "
                "clip interpreted at %.2f fps%s).",
                src->layers.empty() ? "?" : src->layers[0].display_name.c_str(),
                static_cast<long>(src->image_width),
                static_cast<long>(src->image_height),
                frame_count, frame_count == 1 ? "" : "s",
                static_cast<double>(fps), eff_fps,
                (eff_fps > 0.01 && std::fabs(eff_fps - fps) > 0.05)
                    ? " — differs from project fps; check Interpret Footage"
                    : "");
        }
        state->last_status = buf;
    }
    // Tag the new light so it's reachable in the wizard (same as EXR
    // scan). Takes state->mu internally — call outside our lock.
    AutotagByName(state);
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

void RefreshAEProjectName(PanelState* state)
{
    if (!state || !state->pica_basicP) return;
    AEGP_SuiteHandler sp(reinterpret_cast<SPBasicSuite*>(state->pica_basicP));
    AEGP_ProjectH proj = nullptr;
    if (sp.ProjSuite6()->AEGP_GetProjectByIndex(0, &proj) || !proj) return;
    A_char name_buf[AEGP_MAX_PROJ_NAME_SIZE] = {0};
    if (sp.ProjSuite6()->AEGP_GetProjectName(proj, name_buf)) return;
    std::string name = name_buf;
    // Strip a trailing ".aep" / ".aepx" so the session save dialog
    // gets a clean basename to extend (e.g.
    // "MyProject.aep" -> "MyProject"). We just want the project's
    // "stem" — extension is added by the session-save filter.
    auto strip_ext = [](std::string& s, const std::string& ext) {
        if (s.size() <= ext.size()) return false;
        if (s.compare(s.size() - ext.size(), ext.size(), ext) != 0)
            return false;
        s.resize(s.size() - ext.size());
        return true;
    };
    if (!strip_ext(name, ".aepx")) strip_ext(name, ".aep");
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(state->mu);
    state->ae_project_name = std::move(name);
}

void DrainMovieAnalysis(PanelState* state)
{
    if (!state || !state->pica_basicP) return;

    uint32_t source_id = 0;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (state->pending_movie_analysis.empty()) {
            state->want_analyze_movie.store(false);
            return;
        }
        source_id = state->pending_movie_analysis.front();
        state->pending_movie_analysis.erase(
            state->pending_movie_analysis.begin());
    }

    AEGP_SuiteHandler sp(reinterpret_cast<SPBasicSuite*>(state->pica_basicP));
    AEGP_PluginID plugin_id =
        static_cast<AEGP_PluginID>(state->aegp_plugin_id);
    AEGP_ProjectH proj = nullptr;
    if (sp.ProjSuite6()->AEGP_GetProjectByIndex(0, &proj) || !proj) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Clip analysis skipped — no active project.";
        // Leave want_analyze_movie set so we retry on a later tick once
        // a project is available.
        return;
    }

    AnalyzeOneMovie(sp, proj, plugin_id, state, source_id);

    // Clear the "work pending" flag only when the queue is fully drained
    // (one clip per idle tick keeps each render off AE's hot path).
    std::lock_guard<std::mutex> lk(state->mu);
    if (state->pending_movie_analysis.empty()) {
        state->want_analyze_movie.store(false);
    }
}

void AddActiveProjectItem(PanelState* state)
{
    if (!state || !state->pica_basicP) return;
    AEGP_SuiteHandler sp(reinterpret_cast<SPBasicSuite*>(state->pica_basicP));

    AEGP_ItemH item = nullptr;
    if (sp.ItemSuite9()->AEGP_GetActiveItem(&item) || !item) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status =
            "No active Project-panel item — select a footage item first.";
        return;
    }
    AEGP_ItemType type = AEGP_ItemType_NONE;
    sp.ItemSuite9()->AEGP_GetItemType(item, &type);
    if (type != AEGP_ItemType_FOOTAGE) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status =
            "Active item isn't footage — select an EXR / movie / image item.";
        return;
    }
    AEGP_FootageH fh = nullptr;
    if (sp.FootageSuite5()->AEGP_GetMainFootageFromItem(item, &fh) || !fh) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Could not read the active item's footage.";
        return;
    }
    std::string path;
    AEGP_MemHandle hpath = nullptr;
    if (!sp.FootageSuite5()->AEGP_GetFootagePath(
            fh, 0, AEGP_FOOTAGE_MAIN_FILE_INDEX, &hpath) && hpath) {
        path = MemHandleToString(sp, hpath);   // frees the handle
    }
    if (path.empty()) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status =
            "Active item has no file path (a solid or placeholder?).";
        return;
    }
    // Route by extension exactly like a drop: single-light formats
    // (movie/still) take the AE-render analysis path — which re-finds
    // THIS item and reads its STILL/sequence flags + interpretation;
    // .exr goes to the multilayer OpenEXR scanner.
    exr_scan::AddSourcePath(path, state, /*append=*/true, /*source_id=*/0);
}

} // namespace ae_build
