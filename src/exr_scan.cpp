#include "exr_scan.h"

#include "hash.h"
#include "panel_state.h"
#include "diag_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStringAttribute.h>
#include <ImathBox.h>
#include <openexr.h>

namespace exr_scan {

namespace {

// Rec.709 luma weights — same convention as the Python reference.
constexpr float kR709 = 0.2126f;
constexpr float kG709 = 0.7152f;
constexpr float kB709 = 0.0722f;

std::string LowerCopy(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Apply the "X.X" -> "X" doubled-prefix dedup that EXRDemux's
// DisplayNameFromArgbChannel performs. The scanner already strips the
// trailing .R/.G/.B per channel, so this acts on the layer-key level:
//   "World.World"       -> "World"
//   "LightGroup_001"    -> "LightGroup_001"     (no doubled prefix)
//   "Foo.Bar"           -> "Foo.Bar"            (different halves)
std::string DedupDoubledPrefix(const std::string& key)
{
    size_t dot = key.find_last_of('.');
    if (dot == std::string::npos) return key;
    std::string left = key.substr(0, dot);
    std::string right = key.substr(dot + 1);
    if (left == right) return left;
    return key;
}

struct ChannelLookup {
    int   part = -1;
    std::string in_file_name;  // the raw channel name within the part
};

struct RgbGroup {
    std::string   layer_key;     // raw layer key before X.X dedup
    std::string   display_name;  // after dedup; may later be renamed
    // The EXR-internal (post-dedup) name as it lives in the file —
    // what EXRDemux hashes at render. Captured BEFORE our optional
    // single-light filename rename, so the Demux Hi/Lo params stay
    // matchable even after we change display_name.
    std::string   render_name;
    ChannelLookup r, g, b;
    bool          complete = false;
};

// Walk every part / every channel and group channels into per-layer
// R/G/B triples. Channels are grouped by:
//   - last-dot prefix of the channel name (e.g. "World.R" -> "World")
//   - or, if the channel has no dot, by the part's "name" attribute
} // namespace  (SkipReason/EnvironmentHint are part of the API)

// Returns a non-empty reason string if this layer should be skipped
// from chase ordering by default. Empty = include.
//
// Names skipped here aren't pure beauty/lightgroup passes — they're
// either non-luminance data (cryptomattes, hash-encoded), the full-
// frame beauty pass (Image/Alpha — bright everywhere, useless for
// per-light positioning), or environment/ambient passes (World /
// Ambient / HDRI — broad scene illumination rather than a single
// directional light). Matches the disable-by-default list in
// EXRDemux's SplitAndSortPassesToPrecomps.jsx.
std::string SkipReason(const std::string& display)
{
    const std::string low = LowerCopy(display);
    if (low.find("crypto") != std::string::npos) return "cryptomatte";

    // Blender writes its built-in passes into the view-layer part, so
    // they reach us as "ViewLayer.Combined" / "ViewLayer.Noisy Image"
    // while a lightgroup arrives bare. Matching only the whole string
    // let every one of those through into Staging (David, testing
    // USC_CatTowers). Compare the part after the last dot as well.
    std::string tail = low;
    const size_t dot = low.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < low.size()) {
        tail = low.substr(dot + 1);
    }

    struct Rule { const char* name; const char* why; };
    static const Rule kExact[] = {
        // The full-frame beauty pass under its various names. Bright
        // everywhere, so useless for per-light positioning.
        { "image",            "beauty/alpha" },
        { "alpha",            "beauty/alpha" },
        { "combined",         "beauty pass"  },
        { "noisy image",      "beauty pass"  },
        // Broad scene illumination rather than one directional light.
        { "world",            "environment"  },
        { "hdri",             "environment"  },
        { "ambient",          "ambient"      },
        // Cycles denoiser inputs — data, not light. Albedo is the one
        // David hit; Normal and Depth are written alongside it by the
        // same setting and are equally not lights.
        { "denoising albedo", "denoise data" },
        { "denoising normal", "denoise data" },
        { "denoising depth",  "denoise data" },
        // Newer Blender writes these two as well (David, first project
        // of the 2026 season).
        { "denoising specular albedo", "denoise data" },
        { "denoising roughness",       "denoise data" },
    };
    for (const Rule& r : kExact) {
        if (tail == r.name || low == r.name) return r.why;
    }

    // "Combined_Ambient" and friends: Blender's beauty pass qualified
    // by an environment lightgroup. Strip the beauty token and re-test,
    // so the pass is caught WITHOUT turning the environment words into
    // a substring search.
    //
    // That distinction is the whole policy here and it is deliberate:
    // "Ambient_Fill" and "World_Light" are NOT skipped, because a pass
    // name is an arbitrary string and one of those may well be a real
    // fixture. They get a scenery SUGGESTION the artist confirms
    // (EnvironmentHint below). Only a name that also carries a known
    // built-in pass token is skipped outright.
    static const char* const kBeautyPrefixes[] = { "combined", "noisy image" };
    for (const char* pre : kBeautyPrefixes) {
        const size_t n = std::strlen(pre);
        if (tail.size() <= n + 1) continue;
        if (tail.compare(0, n, pre) != 0) continue;
        const char sep = tail[n];
        if (sep != '_' && sep != '-' && sep != ' ' && sep != '.') continue;
        const std::string rest = tail.substr(n + 1);
        for (const Rule& r : kExact) {
            if (rest == r.name) return r.why;
        }
    }
    return {};
}

std::string EnvironmentHint(const std::string& display)
{
    if (!SkipReason(display).empty()) return {};   // already excluded
    const std::string low = LowerCopy(display);
    // Deliberately a short list of unambiguous scenery words. This only
    // ever produces a suggestion the user confirms, but a chatty
    // suggestion is still noise — "sky" is left out precisely because
    // "Skyline_02" is a plausible fixture name.
    struct Hint { const char* needle; const char* why; };
    static const Hint kHints[] = {
        { "world",       "looks like a world pass" },
        { "ambient",     "looks like an ambient pass" },
        { "hdri",        "looks like an HDRI pass" },
        { "environment", "looks like an environment pass" },
        { "ibl",         "looks like an image-based lighting pass" },
    };
    for (const Hint& h : kHints) {
        if (low.find(h.needle) != std::string::npos) return h.why;
    }
    return {};
}

namespace {

std::vector<RgbGroup> BuildRgbGroups(Imf::MultiPartInputFile& file)
{
    std::map<std::string, RgbGroup> by_key;

    for (int p = 0; p < file.parts(); ++p) {
        const Imf::Header& hdr = file.header(p);

        std::string part_name;
        if (hdr.hasName()) {
            part_name = hdr.name();
        } else if (auto* a = hdr.findTypedAttribute<Imf::StringAttribute>("name")) {
            part_name = a->value();
        }

        const Imf::ChannelList& channels = hdr.channels();
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            std::string raw = it.name();

            // Decompose the channel name into (layer_key, sub) where sub
            // is the trailing "R"/"G"/"B"/"A"/"Y"/etc. token.
            std::string layer_key;
            std::string sub;
            size_t dot = raw.find_last_of('.');
            if (dot != std::string::npos) {
                layer_key = raw.substr(0, dot);
                sub = raw.substr(dot + 1);
            } else {
                layer_key = part_name.empty() ? raw : part_name;
                sub = raw;
            }

            // Normalize R/G/B detection (rare lowercase variants).
            std::string sub_upper = sub;
            std::transform(sub_upper.begin(), sub_upper.end(), sub_upper.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (sub_upper != "R" && sub_upper != "G" && sub_upper != "B") {
                continue;  // ignore A, Y, Z, custom suffixes for centroid
            }

            // Disambiguate by part index so two parts with the same
            // layer_key don't collide. We still display the dedup'd
            // layer_key (most common case: parts have unique names).
            std::string map_key = std::to_string(p) + "::" + layer_key;
            RgbGroup& grp = by_key[map_key];
            if (grp.layer_key.empty()) {
                grp.layer_key = layer_key;
                grp.display_name = DedupDoubledPrefix(layer_key);
                // EXR-internal name = the dedup'd display before any
                // ChaseMaker rename. Used for the EXRDemux hash.
                grp.render_name  = grp.display_name;
            }
            ChannelLookup lookup{ p, raw };
            if (sub_upper == "R") grp.r = lookup;
            else if (sub_upper == "G") grp.g = lookup;
            else if (sub_upper == "B") grp.b = lookup;
        }
    }

    std::vector<RgbGroup> out;
    out.reserve(by_key.size());
    for (auto& [_, grp] : by_key) {
        grp.complete = (grp.r.part >= 0 && grp.g.part >= 0 && grp.b.part >= 0);
        out.push_back(std::move(grp));
    }
    return out;
}

// Human-readable EXR compression name — surfaced in skip reasons so a
// decode failure points at the codec (a tester's "Unable to run
// decoder" is far more actionable as "decoder error [DWAB]").
const char* CompressionName(Imf::Compression c)
{
    switch (c) {
        case Imf::NO_COMPRESSION:    return "none";
        case Imf::RLE_COMPRESSION:   return "RLE";
        case Imf::ZIPS_COMPRESSION:  return "ZIPS";
        case Imf::ZIP_COMPRESSION:   return "ZIP";
        case Imf::PIZ_COMPRESSION:   return "PIZ";
        case Imf::PXR24_COMPRESSION: return "PXR24";
        case Imf::B44_COMPRESSION:   return "B44";
        case Imf::B44A_COMPRESSION:  return "B44A";
        case Imf::DWAA_COMPRESSION:  return "DWAA";
        case Imf::DWAB_COMPRESSION:  return "DWAB";
        default:                     return "unknown";
    }
}

std::string PartCompression(Imf::MultiPartInputFile& file, int part)
{
    try { return CompressionName(file.header(part).compression()); }
    catch (...) { return "?"; }
}

// Read R+G+B for one layer as float32 buffers. Returns false on any
// OpenEXR / I/O failure.
// OpenEXR 3.4's DWAA/DWAB decoder breaks when one decode pipeline is
// reused across chunks: the first chunk decodes, every later one fails
// with "Unable to run decoder" (reproduced standalone on a 290-channel
// single-part Blender EXR). The C++ reader reuses its pipeline whenever
// it runs single-threaded, so scanline parts are read here through the
// Core API with a FRESH pipeline per chunk. Returns false (caller falls
// back to the C++ reader) for layouts this doesn't handle: tiled/deep
// parts or subsampled channels. Decode errors throw, like the C++ path.
//
// Takes a BATCH of groups from one part: DWA decompresses a whole chunk
// (every channel in the part) no matter how few channels are wanted, so
// a 290-channel render read one light at a time decompressed the whole
// file once per light (~5 min for 85 lights). Each chunk is decoded
// once per batch instead. bufs[i] is the R/G/B triple for grps[i],
// each pointing at w*h floats.
bool ReadRgbPartsCore(const std::string& path,
                      const std::vector<const RgbGroup*>& grps,
                      int w, int h, int min_y,
                      const std::vector<std::array<float*, 3>>& bufs)
{
    if (grps.empty()) return false;
    struct Ctx {
        exr_context_t c = nullptr;
        ~Ctx() { if (c) exr_finish(&c); }
    } ctx;
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    if (exr_start_read(&ctx.c, path.c_str(), &init) != EXR_ERR_SUCCESS)
        return false;

    const int part = grps[0]->r.part;
    std::map<std::string, float*> targets;   // channel name -> buffer
    for (size_t i = 0; i < grps.size(); ++i) {
        const RgbGroup& g = *grps[i];
        if (g.r.part != part || g.g.part != part || g.b.part != part)
            return false;
        targets[g.r.in_file_name] = bufs[i][0];
        targets[g.g.in_file_name] = bufs[i][1];
        targets[g.b.in_file_name] = bufs[i][2];
    }

    exr_storage_t storage;
    exr_compression_t comp;
    int32_t lines_per_chunk = 0, chunk_count = 0;
    if (exr_get_compression(ctx.c, part, &comp) != EXR_ERR_SUCCESS ||
        (comp != EXR_COMPRESSION_DWAA && comp != EXR_COMPRESSION_DWAB) ||
        exr_get_storage(ctx.c, part, &storage) != EXR_ERR_SUCCESS ||
        storage != EXR_STORAGE_SCANLINE ||
        exr_get_scanlines_per_chunk(ctx.c, part, &lines_per_chunk) != EXR_ERR_SUCCESS ||
        exr_get_chunk_count(ctx.c, part, &chunk_count) != EXR_ERR_SUCCESS ||
        lines_per_chunk <= 0)
        return false;

    for (int ci = 0; ci < chunk_count; ++ci) {
        exr_chunk_info_t cinfo;
        if (exr_read_scanline_chunk_info(ctx.c, part, min_y + ci * lines_per_chunk,
                                         &cinfo) != EXR_ERR_SUCCESS)
            throw std::runtime_error("Unable to read chunk table");

        exr_decode_pipeline_t dec = EXR_DECODE_PIPELINE_INITIALIZER;
        struct Pipe {
            exr_context_t c; exr_decode_pipeline_t* d;
            ~Pipe() { exr_decoding_destroy(c, d); }
        } pipe{ctx.c, &dec};
        if (exr_decoding_initialize(ctx.c, part, &cinfo, &dec) != EXR_ERR_SUCCESS)
            throw std::runtime_error("Unable to initialize decoder");

        const int row0 = cinfo.start_y - min_y;
        size_t found = 0;
        for (int k = 0; k < dec.channel_count; ++k) {
            exr_coding_channel_info_t& ch = dec.channels[k];
            ch.decode_to_ptr = nullptr;
            auto it = targets.find(ch.channel_name);
            if (it == targets.end()) continue;
            if (ch.x_samples != 1 || ch.y_samples != 1 || ch.width != w)
                return false;
            ch.decode_to_ptr = reinterpret_cast<uint8_t*>(
                it->second + static_cast<size_t>(row0) * w);
            ch.user_pixel_stride      = sizeof(float);
            ch.user_line_stride       = static_cast<int32_t>(sizeof(float) * w);
            ch.user_bytes_per_element = sizeof(float);
            ch.user_data_type         = EXR_PIXEL_FLOAT;
            ++found;
        }
        if (found != targets.size() || row0 < 0 || row0 + cinfo.height > h)
            return false;

        if (exr_decoding_choose_default_routines(ctx.c, part, &dec) != EXR_ERR_SUCCESS ||
            exr_decoding_run(ctx.c, part, &dec) != EXR_ERR_SUCCESS)
            throw std::runtime_error("Unable to run decoder");
    }
    return true;
}

bool ReadRgbPart(Imf::MultiPartInputFile& file, const std::string& path,
                 const RgbGroup& grp,
                 int& w_out, int& h_out,
                 std::vector<float>& r_buf,
                 std::vector<float>& g_buf,
                 std::vector<float>& b_buf)
{
    Imf::InputPart input_part(file, grp.r.part);
    const Imath::Box2i dw = file.header(grp.r.part).dataWindow();
    const int w = dw.max.x - dw.min.x + 1;
    const int h = dw.max.y - dw.min.y + 1;
    if (w <= 0 || h <= 0) return false;

    r_buf.assign(static_cast<size_t>(w) * h, 0.0f);
    g_buf.assign(static_cast<size_t>(w) * h, 0.0f);
    b_buf.assign(static_cast<size_t>(w) * h, 0.0f);

    if (ReadRgbPartsCore(path, {&grp}, w, h, dw.min.y,
                         {{r_buf.data(), g_buf.data(), b_buf.data()}})) {
        w_out = w;
        h_out = h;
        return true;
    }

    // OpenEXR slice base must point at image coord (0,0). Our buffers
    // are indexed by (y - dw.min.y) * w + (x - dw.min.x), so the base
    // is `data - (dw.min.x + dw.min.y * w) * sizeof(float)`.
    const ptrdiff_t origin_offset =
        -(static_cast<ptrdiff_t>(dw.min.x) +
          static_cast<ptrdiff_t>(dw.min.y) * w) *
        static_cast<ptrdiff_t>(sizeof(float));

    Imf::FrameBuffer fb;
    fb.insert(grp.r.in_file_name.c_str(), Imf::Slice(
        Imf::FLOAT,
        reinterpret_cast<char*>(r_buf.data()) + origin_offset,
        sizeof(float), sizeof(float) * w));
    fb.insert(grp.g.in_file_name.c_str(), Imf::Slice(
        Imf::FLOAT,
        reinterpret_cast<char*>(g_buf.data()) + origin_offset,
        sizeof(float), sizeof(float) * w));
    fb.insert(grp.b.in_file_name.c_str(), Imf::Slice(
        Imf::FLOAT,
        reinterpret_cast<char*>(b_buf.data()) + origin_offset,
        sizeof(float), sizeof(float) * w));

    input_part.setFrameBuffer(fb);
    input_part.readPixels(dw.min.y, dw.max.y);

    w_out = w;
    h_out = h;
    return true;
}

// Box-filter downsample R/G/B float buffers into a normalized RGBA8
// thumbnail. Aspect-preserving — height scales from `target_max_w`.
// We normalize by the layer's own peak channel so weak lights remain
// visible at preview scale; this trades off realistic relative
// intensities for "can the user see the spatial position at all".
void GenerateThumbnail(const std::vector<float>& r,
                       const std::vector<float>& g,
                       const std::vector<float>& b,
                       int w, int h,
                       int target_max_w,
                       std::vector<uint8_t>& out_rgba,
                       int& out_w, int& out_h,
                       float& out_peak)
{
    out_peak = 0.f;
    if (target_max_w < 1) target_max_w = 1;
    out_w = std::min(target_max_w, w);
    if (out_w < 1) out_w = 1;
    out_h = std::max(1, h * out_w / std::max(1, w));

    // Find max channel value for per-layer normalization. Negative
    // pixels (float ringing) are clamped to 0.
    float max_v = 0.f;
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        float rv = std::max(0.f, r[i]);
        float gv = std::max(0.f, g[i]);
        float bv = std::max(0.f, b[i]);
        if (rv > max_v) max_v = rv;
        if (gv > max_v) max_v = gv;
        if (bv > max_v) max_v = bv;
    }
    const float inv_max = max_v > 0.f ? 1.0f / max_v : 0.f;
    out_peak = max_v;

    out_rgba.assign(static_cast<size_t>(out_w) * out_h * 4, 0);

    // For each output pixel, average the corresponding box of inputs.
    for (int oy = 0; oy < out_h; ++oy) {
        const int y0 = (oy * h) / out_h;
        const int y1 = std::max(y0 + 1, ((oy + 1) * h) / out_h);
        for (int ox = 0; ox < out_w; ++ox) {
            const int x0 = (ox * w) / out_w;
            const int x1 = std::max(x0 + 1, ((ox + 1) * w) / out_w);

            double sum_r = 0, sum_g = 0, sum_b = 0;
            int count = 0;
            for (int yi = y0; yi < y1; ++yi) {
                const size_t row = static_cast<size_t>(yi) * w;
                for (int xi = x0; xi < x1; ++xi) {
                    sum_r += std::max(0.f, r[row + xi]);
                    sum_g += std::max(0.f, g[row + xi]);
                    sum_b += std::max(0.f, b[row + xi]);
                    ++count;
                }
            }
            if (count > 0) {
                sum_r /= count;
                sum_g /= count;
                sum_b /= count;
            }
            // Per-layer normalize, then gamma 2.0 (sqrt) as a cheap
            // linear -> sRGB-ish encoding.
            float fr = static_cast<float>(sum_r) * inv_max;
            float fg = static_cast<float>(sum_g) * inv_max;
            float fb = static_cast<float>(sum_b) * inv_max;
            if (fr < 0.f) fr = 0.f; else if (fr > 1.f) fr = 1.f;
            if (fg < 0.f) fg = 0.f; else if (fg > 1.f) fg = 1.f;
            if (fb < 0.f) fb = 0.f; else if (fb > 1.f) fb = 1.f;
            const auto pack = [](float v) {
                int n = static_cast<int>(std::sqrt(v) * 255.f + 0.5f);
                return static_cast<uint8_t>(n < 0 ? 0 : (n > 255 ? 255 : n));
            };
            const size_t oi = (static_cast<size_t>(oy) * out_w + ox) * 4;
            out_rgba[oi + 0] = pack(fr);
            out_rgba[oi + 1] = pack(fg);
            out_rgba[oi + 2] = pack(fb);
            out_rgba[oi + 3] = 255;
        }
    }
}

// Metrics computed from one pass over the R/G/B buffers:
//   - total luminance
//   - whole-layer luminance-weighted centroid (cx, cy)
//   - hotspot centroid: luminance-weighted centroid restricted to
//     pixels at or above 50% of the peak luminance. Snaps to the
//     bright concentrated source and ignores soft spill.
//   - single brightest pixel position + value
//
// Two passes total: first finds peak, second accumulates the
// thresholded centroid alongside the whole-layer one. Each pass is
// O(w*h) and memory-bandwidth-limited, so doing them together is
// barely more expensive than the single centroid we used to do.
struct ScanMetrics {
    double total = 0.0;
    float  cx = 0.f, cy = 0.f;
    float  cx_hot = 0.f, cy_hot = 0.f;
    float  peak_x = 0.f, peak_y = 0.f;
    float  peak_lum = 0.f;
};

ScanMetrics ComputeMetrics(const std::vector<float>& r,
                           const std::vector<float>& g,
                           const std::vector<float>& b,
                           int w, int h)
{
    ScanMetrics m;
    if (w <= 1 || h <= 1) return m;

    // First pass: peak luminance + whole-layer centroid + total.
    double sum_L = 0.0, sum_Lx = 0.0, sum_Ly = 0.0;
    float  peak = 0.f;
    int    peak_ix = 0, peak_iy = 0;

    for (int y = 0; y < h; ++y) {
        double row_sum = 0.0, row_sum_x = 0.0;
        const size_t row = static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float lum = kR709 * r[row + x] + kG709 * g[row + x] + kB709 * b[row + x];
            if (lum < 0.f) lum = 0.f;
            row_sum   += lum;
            row_sum_x += static_cast<double>(lum) * x;
            if (lum > peak) { peak = lum; peak_ix = x; peak_iy = y; }
        }
        sum_L  += row_sum;
        sum_Lx += row_sum_x;
        sum_Ly += row_sum * y;
    }

    m.total    = sum_L;
    m.peak_lum = peak;
    m.peak_x   = static_cast<float>(peak_ix) / std::max(1, w - 1);
    m.peak_y   = static_cast<float>(peak_iy) / std::max(1, h - 1);

    if (sum_L > 0.0) {
        m.cx = static_cast<float>(sum_Lx / sum_L / std::max(1, w - 1));
        m.cy = static_cast<float>(sum_Ly / sum_L / std::max(1, h - 1));
    }

    // Second pass: hotspot centroid (>= 50% of peak). Falls back to
    // the peak pixel if too few qualifying pixels exist.
    if (peak > 0.f) {
        const float thresh = peak * 0.5f;
        double hs_L = 0.0, hs_Lx = 0.0, hs_Ly = 0.0;
        for (int y = 0; y < h; ++y) {
            const size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                float lum = kR709 * r[row + x] + kG709 * g[row + x] + kB709 * b[row + x];
                if (lum < thresh) continue;
                hs_L  += lum;
                hs_Lx += static_cast<double>(lum) * x;
                hs_Ly += static_cast<double>(lum) * y;
            }
        }
        if (hs_L > 0.0) {
            m.cx_hot = static_cast<float>(hs_Lx / hs_L / std::max(1, w - 1));
            m.cy_hot = static_cast<float>(hs_Ly / hs_L / std::max(1, h - 1));
        } else {
            m.cx_hot = m.peak_x;
            m.cy_hot = m.peak_y;
        }
    } else {
        m.cx_hot = m.cx;
        m.cy_hot = m.cy;
    }
    return m;
}

namespace fs = std::filesystem;

bool HasExrExt(const fs::path& p)
{
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".exr";
}

// Files in `dir` whose name is exactly prefix + <digits> + suffix.
// prefix/suffix are matched LITERALLY (pass/sequence names are
// arbitrary strings — no regex, per project rules). Sorted by the
// integer value of the digit run so frame 1 is index 0.
std::vector<std::string> CollectNumberedSiblings(
    const fs::path& dir, const std::string& prefix, const std::string& suffix)
{
    std::vector<std::pair<long long, std::string>> hits;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (!suffix.empty() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string mid = name.substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        if (mid.empty()) continue;
        bool digits = true;
        for (unsigned char c : mid)
            if (!std::isdigit(c)) { digits = false; break; }
        if (!digits) continue;
        long long n = 0;
        try { n = std::stoll(mid); } catch (...) { continue; }
        hits.emplace_back(n, it->path().string());
    }
    std::sort(hits.begin(), hits.end(),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
    std::vector<std::string> out;
    out.reserve(hits.size());
    for (auto& h : hits) out.push_back(std::move(h.second));
    return out;
}

// Split a bare filename into (prefix, suffix) around the LAST run of
// digits (the frame counter). False = no digit run (standalone
// still). The variable part is ALWAYS the pure-digit run, so
// `TestFile_###` and `TestFile_Test_###` resolve as distinct
// sequences (the latter's mid would be "Test_###", not all digits).
bool SplitSeqName(const std::string& fn, std::string& prefix,
                  std::string& suffix)
{
    size_t end = std::string::npos, beg = std::string::npos;
    for (size_t i = fn.size(); i-- > 0; ) {
        if (std::isdigit(static_cast<unsigned char>(fn[i]))) {
            if (end == std::string::npos) end = i + 1;
            beg = i;
        } else if (end != std::string::npos) {
            break;
        }
    }
    if (end == std::string::npos) return false;
    prefix = fn.substr(0, beg);
    suffix = fn.substr(end);
    return true;
}

// Turn whatever path AE / Explorer hands us (a concrete frame file, a
// directory of frames, or a sequence-token path like name_[0001-0060]
// .exr / name_####.exr / name_%04d.exr) into a single concrete EXR
// file to actually open. `frame_index` (0-based) picks which frame of
// the discovered sequence to use; it is clamped into range. Returns
// false + a human-readable reason on failure.
bool ResolveExrFrame(const std::string& input, int frame_index,
                     std::string& out_file, int& out_count,
                     int& out_used_index, std::string& out_err,
                     bool* out_asked_for_sequence)
{
    // Whether the INPUT asked for a sequence (a folder, or a ####/%04d
    // token) as opposed to naming one concrete file. Sibling frames are
    // still discovered either way — that is how the Sources tab can
    // offer to treat a single dropped frame as an animation — but
    // dropping shot_0001.exr must not silently make it one.
    if (out_asked_for_sequence) *out_asked_for_sequence = false;
    std::error_code ec;
    fs::path in(input);
    std::vector<std::string> frames;

    if (fs::is_directory(in, ec)) {
        if (out_asked_for_sequence) *out_asked_for_sequence = true;
        // A folder can hold MULTIPLE distinct sequences (e.g.
        // TestFile_### alongside TestFile_Test_###). Don't merge
        // them: take the alphabetically-first .exr as the reference
        // and collect ONLY its numbered siblings — a differently
        // named sequence's frames are excluded.
        std::vector<std::string> all;
        for (fs::directory_iterator it(in, ec), e2; it != e2 && !ec;
             it.increment(ec)) {
            if (it->is_regular_file(ec) && HasExrExt(it->path()))
                all.push_back(it->path().filename().string());
        }
        if (!all.empty()) {
            std::sort(all.begin(), all.end());
            std::string pfx, sfx;
            if (SplitSeqName(all.front(), pfx, sfx))
                frames = CollectNumberedSiblings(in, pfx, sfx);
            if (frames.empty())
                frames.push_back((in / all.front()).string());
        }
    } else if (fs::is_regular_file(in, ec)) {
        // Derive the sequence from this frame: the LAST run of digits
        // is the frame counter; the fixed prefix/suffix pin it to
        // exactly this sequence.
        const std::string fn = in.filename().string();
        std::string pfx, sfx;
        if (!SplitSeqName(fn, pfx, sfx)) {
            frames.push_back(in.string());          // standalone, no number
        } else {
            frames = CollectNumberedSiblings(in.parent_path(), pfx, sfx);
            if (frames.empty()) frames.push_back(in.string());
        }
    } else {
        // Doesn't exist as-is: treat the filename as a sequence token.
        if (out_asked_for_sequence) *out_asked_for_sequence = true;
        fs::path parent = in.parent_path();
        if (!fs::is_directory(parent, ec)) {
            out_err = "Path not found: " + input;
            return false;
        }
        const std::string fn = in.filename().string();
        size_t tpos = std::string::npos;
        for (size_t i = 0; i < fn.size(); ++i) {
            char c = fn[i];
            if (c == '#' || c == '*' || c == '%' || c == '[') { tpos = i; break; }
        }
        if (tpos == std::string::npos) {
            out_err = "File not found: " + input;
            return false;
        }
        std::string prefix = fn.substr(0, tpos);
        size_t after = tpos;
        if (fn[tpos] == '[') {
            size_t close = fn.find(']', tpos);
            if (close == std::string::npos) {
                out_err = "Unrecognized sequence pattern: " + fn;
                return false;
            }
            after = close + 1;
        } else if (fn[tpos] == '#') {
            while (after < fn.size() && fn[after] == '#') ++after;
        } else if (fn[tpos] == '*') {
            after = tpos + 1;
        } else { // '%' printf-style
            after = tpos + 1;
            while (after < fn.size() &&
                   std::isdigit(static_cast<unsigned char>(fn[after]))) ++after;
            if (after < fn.size() &&
                (fn[after] == 'd' || fn[after] == 'D' || fn[after] == 'i'))
                ++after;
        }
        frames = CollectNumberedSiblings(parent, prefix, fn.substr(after));
    }

    if (frames.empty()) {
        out_err = "No EXR frames found for: " + input;
        return false;
    }
    int idx = frame_index < 0 ? 0 : frame_index;
    if (idx >= static_cast<int>(frames.size()))
        idx = static_cast<int>(frames.size()) - 1;
    out_file       = frames[static_cast<size_t>(idx)];
    out_count      = static_cast<int>(frames.size());
    out_used_index = idx;
    return true;
}

void DoScan(const std::string& input, PanelState* state,
            bool append, uint32_t fixed_source_id, int frame_index)
{
    std::string path;
    int frame_count = 1, used_frame = 0;
    bool asked_for_sequence = false;
    {
        std::string rerr;
        if (!ResolveExrFrame(input, frame_index, path, frame_count,
                              used_frame, rerr, &asked_for_sequence)) {
            CM_DIAG_LOG("DoScan: resolve failed input='%s' err='%s'",
                        input.c_str(), rerr.c_str());
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error  = "EXR scan failed: " + rerr;
            state->last_status = "Scan failed.";
            if (!append) {
                state->sources.clear();
                state->active_source_index = -1;
            }
            return;
        }
    }
    CM_DIAG_LOG("DoScan: input='%s' -> frame %d/%d '%s'",
                input.c_str(), used_frame + 1, frame_count, path.c_str());

    try {
        Imf::MultiPartInputFile file(path.c_str());

        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_status = "Enumerating layers...";
        }

        std::vector<RgbGroup> groups = BuildRgbGroups(file);

        // Single-light source: rename the lone real layer to the
        // FILENAME base (frame digits stripped) so multi-file
        // workflows like SpeakersRowGroup1.exr / 2.exr / ... produce
        // differentiated per-file layer names that the auto-tag-by-
        // prefix groups together ("SpeakersRowGroup"). Counted
        // AFTER the skip list (cryptomattes, Image/Alpha, World,
        // Ambient, HDRI, RGB-incomplete) — Blender exports with
        // cryptomatte naturally have multiple raw groups but only
        // one "real" light layer. Multilayer EXRs (2+ real layers)
        // are untouched; their internal names self-describe.
        {
            int effective = 0;
            RgbGroup* sole = nullptr;
            for (auto& g : groups) {
                if (!g.complete) continue;
                if (!SkipReason(g.display_name).empty()) continue;
                ++effective;
                sole = &g;
            }
            if (effective == 1 && sole) {
                fs::path p(path);
                std::string base = p.stem().string();
                if (frame_count > 1) {
                    size_t e = base.size();
                    while (e > 0 && std::isdigit(
                               static_cast<unsigned char>(base[e - 1]))) --e;
                    while (e > 0 && (base[e - 1] == '_' || base[e - 1] == '-' ||
                                      base[e - 1] == '.' || base[e - 1] == ' '))
                        --e;
                    if (e > 0) base.resize(e);
                }
                if (!base.empty()) sole->display_name = base;
            }
        }

        // Use the data window of part 0 as the headline image size.
        int img_w = 0, img_h = 0;
        if (file.parts() > 0) {
            const Imath::Box2i dw = file.header(0).dataWindow();
            img_w = dw.max.x - dw.min.x + 1;
            img_h = dw.max.y - dw.min.y + 1;
        }

        std::vector<LayerInfo> layers;
        std::vector<SkippedLayer> skipped;
        layers.reserve(groups.size());

        std::vector<float> r_buf, g_buf, b_buf;
        size_t scanned_idx = 0;
        const size_t scan_total = groups.size();

        // DWA parts decompress every channel per chunk, so lights that
        // share one are decoded together in batches (ReadRgbPartsCore)
        // and handed to the per-light loop below from `prefetched`.
        // Bounded by kBatchBytes of float planes; a batch that fails is
        // dropped and its lights fall back to the one-at-a-time read,
        // which records each light's own error.
        constexpr size_t kBatchBytes = 768ull << 20;
        std::map<size_t, std::array<std::vector<float>, 3>> prefetched;
        std::set<int> no_batch_parts;
        auto scannable = [](const RgbGroup& g) {
            return g.complete && SkipReason(g.display_name).empty();
        };
        auto prefetch_from = [&](size_t gi) {
            const int part = groups[gi].r.part;
            if (no_batch_parts.count(part)) return;
            const Imf::Compression c = file.header(part).compression();
            if (c != Imf::DWAA_COMPRESSION && c != Imf::DWAB_COMPRESSION) {
                no_batch_parts.insert(part);
                return;
            }
            const Imath::Box2i dw = file.header(part).dataWindow();
            const int w = dw.max.x - dw.min.x + 1;
            const int h = dw.max.y - dw.min.y + 1;
            if (w <= 0 || h <= 0) return;
            const size_t light_bytes = static_cast<size_t>(w) * h * sizeof(float) * 3;
            const size_t k_max = kBatchBytes / light_bytes;
            std::vector<size_t> idx;
            for (size_t j = gi; j < groups.size() && idx.size() < k_max; ++j)
                if (scannable(groups[j]) && groups[j].r.part == part &&
                    groups[j].g.part == part && groups[j].b.part == part)
                    idx.push_back(j);
            if (idx.size() < 2) return;   // nothing to share

            std::vector<std::array<std::vector<float>, 3>> planes(idx.size());
            std::vector<const RgbGroup*> grps;
            std::vector<std::array<float*, 3>> ptrs;
            for (size_t i = 0; i < idx.size(); ++i) {
                for (auto& p : planes[i]) p.assign(static_cast<size_t>(w) * h, 0.f);
                grps.push_back(&groups[idx[i]]);
                ptrs.push_back({planes[i][0].data(), planes[i][1].data(),
                                planes[i][2].data()});
            }
            bool ok = false;
            try {
                ok = ReadRgbPartsCore(path, grps, w, h, dw.min.y, ptrs);
            } catch (const std::exception& e) {
                CM_DIAG_LOG("scan: batch of %d on part %d failed: %s",
                            (int)idx.size(), part, e.what());
            }
            if (!ok) { no_batch_parts.insert(part); return; }
            for (size_t i = 0; i < idx.size(); ++i)
                prefetched[idx[i]] = std::move(planes[i]);
        };

        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const RgbGroup& grp = groups[gi];
            ++scanned_idx;
            {
                std::lock_guard<std::mutex> lk(state->mu);
                state->last_status =
                    "Scanning " + std::to_string(scanned_idx) +
                    "/" + std::to_string(scan_total) + ": " + grp.display_name;
            }

            if (!grp.complete) {
                skipped.push_back({grp.display_name, "not RGB-complete"});
                continue;
            }
            if (std::string reason = SkipReason(grp.display_name); !reason.empty()) {
                skipped.push_back({grp.display_name, reason});
                continue;
            }

            int w = 0, h = 0;
            if (!prefetched.count(gi)) prefetch_from(gi);
            if (auto it = prefetched.find(gi); it != prefetched.end()) {
                const Imath::Box2i dw = file.header(grp.r.part).dataWindow();
                w = dw.max.x - dw.min.x + 1;
                h = dw.max.y - dw.min.y + 1;
                r_buf = std::move(it->second[0]);
                g_buf = std::move(it->second[1]);
                b_buf = std::move(it->second[2]);
                prefetched.erase(it);
            } else try {
                if (!ReadRgbPart(file, path, grp, w, h, r_buf, g_buf, b_buf)) {
                    skipped.push_back({grp.display_name, "read failed"});
                    continue;
                }
            } catch (const std::exception& e) {
                // One undecodable part must NOT abort the whole scan.
                // OpenEXR throws here (e.g. "Unable to run decoder")
                // on an unsupported/corrupt compressed part or a
                // not-fully-materialized file. Record it with the
                // part's compression and keep scanning the rest, so
                // the user still gets every layer that DOES decode
                // and a precise, actionable reason for the rest.
                const std::string comp = PartCompression(file, grp.r.part);
                CM_DIAG_LOG("scan: part %d '%s' decode failed [%s]: %s",
                            grp.r.part, grp.display_name.c_str(),
                            comp.c_str(), e.what());
                skipped.push_back({grp.display_name,
                    "decoder error [" + comp + "]: " + e.what()});
                continue;
            }

            ScanMetrics m = ComputeMetrics(r_buf, g_buf, b_buf, w, h);
            if (m.total <= 0.0) {
                skipped.push_back({grp.display_name, "all black"});
                continue;
            }

            LayerInfo info;
            info.display_name   = grp.display_name;
            info.exr_layer_name = grp.render_name;        // EXR-internal name (for Demux hash)
            info.fnv1a_hash     = FNV1a32(grp.display_name);
            info.cx           = m.cx;
            info.cy           = m.cy;
            info.cx_hot       = m.cx_hot;
            info.cy_hot       = m.cy_hot;
            info.peak_x       = m.peak_x;
            info.peak_y       = m.peak_y;
            info.peak_lum     = m.peak_lum;
            info.total        = m.total;

            // Thumbnail downsample from the same R/G/B buffers we
            // just centroided. Snapshot the user-requested width
            // once outside the per-layer hot loop so a slider tweak
            // mid-scan doesn't make heights inconsistent across rows.
            int tw = 0, th = 0;
            int target_w = 256;
            {
                std::lock_guard<std::mutex> lk(state->mu);
                target_w = state->thumb_max_width;
            }
            GenerateThumbnail(r_buf, g_buf, b_buf, w, h, target_w,
                              info.thumb_rgba, tw, th, info.thumb_peak);
            info.thumb_w = tw;
            info.thumb_h = th;

            layers.push_back(std::move(info));
        }

        // Initial sort uses the current UI sort settings so a re-scan
        // doesn't surprise the user with a different order. The UI
        // can re-sort live without re-scanning.
        PanelState::SortMode mode;
        bool reverse;
        uint32_t seed;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            mode    = state->sort_mode;
            reverse = state->sort_reverse;
            seed    = state->random_seed;
        }
        SortLayers(layers, mode, reverse, seed);

        {
            std::lock_guard<std::mutex> lk(state->mu);
            Source src;
            if (fixed_source_id != 0) {
                src.source_id = fixed_source_id;
                if (state->next_source_id <= fixed_source_id) {
                    state->next_source_id = fixed_source_id + 1;
                }
            } else {
                src.source_id = state->next_source_id++;
            }
            src.path         = input;        // original drag/pick path
            src.scan_path    = path;         // concrete frame opened
            src.scan_frame   = used_frame;
            src.frame_count  = frame_count;
            // Animation ONLY when the source was named as one: a
            // folder, or a ####/%04d token. Dropping one concrete file
            // means that file, even when numbered siblings sit beside
            // it — a single reference frame pulled out of a rendered
            // sequence was being turned into a 3-frame animation, and
            // every chase got crammed into a 3-frame loop.
            // frame_count still reports the siblings, so the Sources
            // tab can offer to treat it as an animation after all.
            src.animation    = asked_for_sequence && (frame_count > 1);
            src.image_width  = img_w;
            src.image_height = img_h;
            src.layers       = std::move(layers);
            src.skipped      = std::move(skipped);
            if (!append) {
                state->sources.clear();
            }
            // Replace an existing source with the same id in place (a
            // frame re-scan) so we don't stack duplicates and the row
            // keeps its position / active selection. Otherwise append.
            int replaced = -1;
            if (fixed_source_id != 0) {
                for (size_t i = 0; i < state->sources.size(); ++i) {
                    if (state->sources[i].source_id == fixed_source_id) {
                        // Preserve the user's animation override across
                        // a frame re-scan (don't reset to auto-default).
                        src.animation = state->sources[i].animation;
                        state->sources[i] = std::move(src);
                        replaced = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (replaced < 0) {
                state->sources.push_back(std::move(src));
                state->active_source_index = (int)state->sources.size() - 1;
            } else {
                state->active_source_index = replaced;
            }
            state->last_error.clear();
            state->last_status  = frame_count > 1
                ? ("Scan complete (frame " + std::to_string(used_frame + 1) +
                   "/" + std::to_string(frame_count) + ").")
                : std::string("Scan complete.");
        }
        // Auto-tag-by-name once the source is published. Idempotent
        // against repeated calls (existing tags get new members added,
        // not duplicated). Locks state.mu internally, so call here
        // outside our own publish lock.
        AutotagByName(state);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error  = std::string("EXR scan failed: ") + e.what();
        state->last_status = "Scan failed.";
        if (!append) {
            state->sources.clear();
            state->active_source_index = -1;
        }
    }
}

} // namespace

void StartScan(const std::string& path, PanelState* state,
               bool append, uint32_t source_id, int frame_index)
{
    if (!state) return;
    if (state->scanning.exchange(true)) {
        return;  // already scanning, ignore re-entry
    }
    // Bump generation BEFORE the worker starts mutating state so
    // the platform renderer can drop any previous-scan textures on
    // its next frame before new thumbnails start arriving.
    state->scan_generation.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Opening EXR...";
        state->last_error.clear();
    }
    std::thread([state, path, append, source_id, frame_index]() {
        DoScan(path, state, append, source_id, frame_index);
        state->scanning = false;
    }).detach();
}

void AddSourcePath(const std::string& path, PanelState* state,
                   bool append, uint32_t source_id, int frame_index)
{
    if (!state) return;

    // Route by extension. Self-contained single-light files (movies and
    // still images) can't go through the OpenEXR multilayer reader; they
    // take the AE-render analysis path. Multilayer .exr stays on the
    // OpenEXR scanner (it extracts many light layers per file).
    std::string ext;
    {
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) ext = LowerCopy(path.substr(dot));
    }
    const bool single_light =
        // movies
        ext == ".mov" || ext == ".mp4"  || ext == ".mxf" ||
        // still images (one light each; a PNG sequence imported in AE is
        // detected as an animation at analysis time via the item's flags)
        ext == ".png" || ext == ".tif"  || ext == ".tiff" ||
        ext == ".jpg" || ext == ".jpeg" || ext == ".tga"  ||
        ext == ".dpx" || ext == ".hdr";
    if (!single_light) {
        StartScan(path, state, append, source_id, frame_index);
        return;
    }

    // Single-light: create a placeholder Source immediately so it shows
    // in the Sources tab, then queue it for AE-render analysis (idle hook
    // → ae_build::DrainMovieAnalysis) which reads the AE item to set
    // still-vs-animation, dimensions, frame_count, centroid metrics, and
    // thumbnail. We do NOT bump scan_generation (that tears down other
    // sources' textures).
    namespace fs = std::filesystem;
    std::string stem = fs::path(path).stem().string();
    if (stem.empty()) stem = "Clip";

    uint32_t new_id = 0;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        Source src;
        if (source_id != 0) {
            src.source_id = source_id;
            if (state->next_source_id <= source_id) {
                state->next_source_id = source_id + 1;
            }
        } else {
            src.source_id = state->next_source_id++;
        }
        new_id = src.source_id;
        src.path        = path;
        src.scan_path   = path;     // the file itself is the "frame"
        src.is_movie    = true;     // AE-decoded single-light source
        src.animation   = false;    // analysis sets it from the AE item
        src.frame_count = 1;        // real count filled in at analysis

        LayerInfo info;
        info.display_name = stem;
        info.fnv1a_hash   = FNV1a32(stem);
        // One file, one light — so the filename IS the pass name, and
        // the same scenery list the EXR scanner applies has to apply
        // here too. A folder of per-light .mov renders routinely
        // contains a World.mov. Excluded rather than hidden: it stays
        // visible in Staging with its tick cleared, because promoting
        // a skipped layer is an EXR-only path.
        info.included     = SkipReason(stem).empty();
        info.cx = info.cy = 0.5f;          // placeholder until analyzed
        info.cx_hot = info.cy_hot = 0.5f;
        src.layers.push_back(std::move(info));

        if (!append) {
            state->sources.clear();
        }
        // Replace an existing source with the same id in place (session
        // restore / re-add) so the row keeps its slot; else append.
        int replaced = -1;
        if (source_id != 0) {
            for (size_t i = 0; i < state->sources.size(); ++i) {
                if (state->sources[i].source_id == source_id) {
                    state->sources[i] = std::move(src);
                    replaced = static_cast<int>(i);
                    break;
                }
            }
        }
        if (replaced < 0) {
            state->sources.push_back(std::move(src));
            state->active_source_index =
                static_cast<int>(state->sources.size()) - 1;
        } else {
            state->active_source_index = replaced;
        }
        state->pending_movie_analysis.push_back(new_id);
        state->last_error.clear();
        state->last_status = "Analyzing clip: " + stem + "...";
    }
    state->want_analyze_movie.store(true);
}

void AnalyzeFramePixels(const std::vector<float>& r,
                        const std::vector<float>& g,
                        const std::vector<float>& b,
                        int w, int h, int thumb_max_w,
                        LayerInfo& out)
{
    // ComputeMetrics / GenerateThumbnail live in the anonymous
    // namespace above; they're visible here in the enclosing exr_scan
    // namespace. Same math the EXR scan uses, fed AE-decoded pixels.
    ScanMetrics m = ComputeMetrics(r, g, b, w, h);
    out.total    = m.total;
    out.cx       = m.cx;     out.cy     = m.cy;
    out.cx_hot   = m.cx_hot; out.cy_hot = m.cy_hot;
    out.peak_x   = m.peak_x; out.peak_y = m.peak_y;
    out.peak_lum = m.peak_lum;

    int tw = 0, th = 0;
    GenerateThumbnail(r, g, b, w, h, thumb_max_w,
                      out.thumb_rgba, tw, th, out.thumb_peak);
    out.thumb_w = tw;
    out.thumb_h = th;
}

bool IncludeSkippedLayer(PanelState* state, uint32_t source_id,
                         const std::string& display_name)
{
    if (!state) return false;

    std::string exr_path;
    int target_thumb_width = 384;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const Source* src = FindSourceById(*state, source_id);
        if (!src) {
            state->last_error = "Source not found.";
            return false;
        }
        exr_path = src->scan_path.empty() ? src->path : src->scan_path;
        target_thumb_width = state->thumb_max_width;
    }
    if (exr_path.empty()) return false;

    try {
        Imf::MultiPartInputFile file(exr_path.c_str());
        std::vector<RgbGroup> groups = BuildRgbGroups(file);

        const RgbGroup* match = nullptr;
        for (const auto& g : groups) {
            if (g.display_name == display_name) { match = &g; break; }
        }
        if (!match) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error = "Layer not found in EXR: " + display_name;
            return false;
        }
        if (!match->complete) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error = "Layer is not RGB-complete: " + display_name;
            return false;
        }

        std::vector<float> r_buf, g_buf, b_buf;
        int w = 0, h = 0;
        if (!ReadRgbPart(file, exr_path, *match, w, h, r_buf, g_buf, b_buf)) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error = "Read failed for layer: " + display_name;
            return false;
        }

        ScanMetrics m = ComputeMetrics(r_buf, g_buf, b_buf, w, h);

        LayerInfo info;
        info.display_name   = display_name;
        info.exr_layer_name = display_name;   // un-renamed path: same as display
        info.fnv1a_hash     = FNV1a32(display_name);
        info.cx           = m.cx;
        info.cy           = m.cy;
        info.cx_hot       = m.cx_hot;
        info.cy_hot       = m.cy_hot;
        info.peak_x       = m.peak_x;
        info.peak_y       = m.peak_y;
        info.peak_lum     = m.peak_lum;
        info.total        = m.total;
        int tw = 0, th = 0;
        GenerateThumbnail(r_buf, g_buf, b_buf, w, h, target_thumb_width,
                          info.thumb_rgba, tw, th, info.thumb_peak);
        info.thumb_w = tw;
        info.thumb_h = th;

        std::lock_guard<std::mutex> lk(state->mu);
        Source* src = FindSourceById(*state, source_id);
        if (!src) return false;
        // Remove from skipped (match by display_name).
        for (auto it = src->skipped.begin(); it != src->skipped.end(); ) {
            if (it->display_name == display_name) it = src->skipped.erase(it);
            else ++it;
        }
        src->layers.push_back(std::move(info));
        // Re-sort with the active mode so the new row lands in the
        // expected position.
        SortLayers(src->layers, state->sort_mode,
                   state->sort_reverse, state->random_seed);
        state->last_status = "Included '" + display_name + "' from skipped list.";
        state->last_error.clear();
        return true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = std::string("Include failed: ") + e.what();
        return false;
    }
}

} // namespace exr_scan
