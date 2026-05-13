#include "exr_scan.h"

#include "hash.h"
#include "panel_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStringAttribute.h>
#include <ImathBox.h>

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

bool ShouldSkipByName(const std::string& display)
{
    std::string low = LowerCopy(display);
    if (low.find("crypto") != std::string::npos) return true;
    if (low == "image" || low == "alpha") return true;
    return false;
}

struct ChannelLookup {
    int   part = -1;
    std::string in_file_name;  // the raw channel name within the part
};

struct RgbGroup {
    std::string   layer_key;     // raw layer key before X.X dedup
    std::string   display_name;  // after dedup
    ChannelLookup r, g, b;
    bool          complete = false;
};

// Walk every part / every channel and group channels into per-layer
// R/G/B triples. Channels are grouped by:
//   - last-dot prefix of the channel name (e.g. "World.R" -> "World")
//   - or, if the channel has no dot, by the part's "name" attribute
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

// Read R+G+B for one layer as float32 buffers. Returns false on any
// OpenEXR / I/O failure.
bool ReadRgbPart(Imf::MultiPartInputFile& file, const RgbGroup& grp,
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

// Centroid = (sum(L*x)/sum(L), sum(L*y)/sum(L)) with L clamped to >= 0.
// Returns total luminance; centroid is written via outparams.
double ComputeCentroid(const std::vector<float>& r,
                       const std::vector<float>& g,
                       const std::vector<float>& b,
                       int w, int h,
                       float& cx_out, float& cy_out)
{
    cx_out = 0.f;
    cy_out = 0.f;
    if (w <= 1 || h <= 1) return 0.0;

    double sum_L = 0.0;
    double sum_Lx = 0.0;
    double sum_Ly = 0.0;

    // Reduce row sums first to keep the inner loop tight on memory.
    for (int y = 0; y < h; ++y) {
        double row_sum = 0.0;
        double row_sum_x = 0.0;
        const size_t row = static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float lum = kR709 * r[row + x] + kG709 * g[row + x] + kB709 * b[row + x];
            if (lum < 0.f) lum = 0.f;  // filter ringing on float EXRs
            row_sum   += lum;
            row_sum_x += static_cast<double>(lum) * x;
        }
        sum_L  += row_sum;
        sum_Lx += row_sum_x;
        sum_Ly += row_sum * y;
    }

    if (sum_L <= 0.0) return 0.0;
    cx_out = static_cast<float>(sum_Lx / sum_L / std::max(1, w - 1));
    cy_out = static_cast<float>(sum_Ly / sum_L / std::max(1, h - 1));
    return sum_L;
}

void DoScan(const std::string& path, PanelState* state)
{
    try {
        Imf::MultiPartInputFile file(path.c_str());

        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_status = "Enumerating layers...";
        }

        std::vector<RgbGroup> groups = BuildRgbGroups(file);

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

        for (const auto& grp : groups) {
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
            if (ShouldSkipByName(grp.display_name)) {
                skipped.push_back({grp.display_name, "crypto/beauty/alpha"});
                continue;
            }

            int w = 0, h = 0;
            if (!ReadRgbPart(file, grp, w, h, r_buf, g_buf, b_buf)) {
                skipped.push_back({grp.display_name, "read failed"});
                continue;
            }

            float cx = 0.f, cy = 0.f;
            double total = ComputeCentroid(r_buf, g_buf, b_buf, w, h, cx, cy);
            if (total <= 0.0) {
                skipped.push_back({grp.display_name, "all black"});
                continue;
            }

            LayerInfo info;
            info.display_name = grp.display_name;
            info.fnv1a_hash   = FNV1a32(grp.display_name);
            info.cx           = cx;
            info.cy           = cy;
            info.total        = total;
            layers.push_back(std::move(info));
        }

        // Sort left-to-right by centroid x.
        std::sort(layers.begin(), layers.end(),
            [](const LayerInfo& a, const LayerInfo& b) { return a.cx < b.cx; });

        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->exr_path     = path;
            state->image_width  = img_w;
            state->image_height = img_h;
            state->layers       = std::move(layers);
            state->skipped      = std::move(skipped);
            state->last_error.clear();
            state->last_status  = "Scan complete.";
            state->sidecar_written = false;
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error  = std::string("EXR scan failed: ") + e.what();
        state->last_status = "Scan failed.";
        state->layers.clear();
        state->skipped.clear();
        state->image_width = state->image_height = 0;
    }
}

// Minimal JSON-string escaper. Covers the cases that can appear in
// EXR layer names (quotes, backslashes, control chars). Output is
// ASCII-safe; non-ASCII bytes pass through unchanged as raw UTF-8.
std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

} // namespace

void StartScan(const std::string& path, PanelState* state)
{
    if (!state) return;
    if (state->scanning.exchange(true)) {
        return;  // already scanning, ignore re-entry
    }
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Opening EXR...";
        state->last_error.clear();
    }
    std::thread([state, path]() {
        DoScan(path, state);
        state->scanning = false;
    }).detach();
}

bool WriteLuminositySidecar(PanelState* state)
{
    if (!state) return false;

    std::string path;
    int w = 0, h = 0;
    std::vector<LayerInfo> layers_copy;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (state->exr_path.empty() || state->layers.empty()) {
            state->last_error = "Nothing to write (no scan results).";
            return false;
        }
        path = state->exr_path;
        w = state->image_width;
        h = state->image_height;
        layers_copy = state->layers;
    }

    namespace fs = std::filesystem;
    const fs::path exr_path = fs::u8path(path);
    const fs::path sidecar_path = fs::u8path(path + ".luminosity.json");
    const std::string source_filename = exr_path.filename().u8string();

    int64_t mtime_unix = 0;
    {
        std::error_code ec;
        auto ftime = fs::last_write_time(exr_path, ec);
        if (!ec) {
            // file_clock -> system_clock conversion is non-portable
            // before C++20; this round-trips via duration since epoch
            // measured by file_clock's own epoch, which is sufficient
            // for staleness-checking by EXRDemux's JSX.
            auto sys_now  = std::chrono::system_clock::now();
            auto file_now = decltype(ftime)::clock::now();
            auto sys_time = std::chrono::time_point_cast<std::chrono::seconds>(
                std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        ftime.time_since_epoch() -
                        (file_now.time_since_epoch() - sys_now.time_since_epoch()))));
            mtime_unix = sys_time.time_since_epoch().count();
        }
    }

    std::ofstream out(sidecar_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = "Could not open sidecar for writing: " +
                            sidecar_path.u8string();
        return false;
    }

    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"source\": \"" << JsonEscape(source_filename) << "\",\n";
    out << "  \"source_mtime_utc\": " << mtime_unix << ",\n";
    out << "  \"image_size\": [" << w << ", " << h << "],\n";
    out << "  \"layers\": {\n";
    for (size_t i = 0; i < layers_copy.size(); ++i) {
        const LayerInfo& L = layers_copy[i];
        out << "    \"" << JsonEscape(L.display_name) << "\": {"
            << "\"cx\": "    << L.cx
            << ", \"cy\": "  << L.cy
            << ", \"total\": " << L.total
            << "}";
        if (i + 1 < layers_copy.size()) out << ",";
        out << "\n";
    }
    out << "  }\n";
    out << "}\n";

    if (!out.good()) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = "Write failed mid-stream.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Sidecar written: " + sidecar_path.u8string();
        state->last_error.clear();
    }
    state->sidecar_written = true;
    return true;
}

} // namespace exr_scan
