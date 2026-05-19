#include "session_io.h"

#include "panel_state.h"
#include "exr_scan.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace session_io {

namespace {

// ===== Tiny JSON value + parser =======================================
//
// Schema-driven, not general-purpose. Handles: null, true/false,
// numbers (int + double), strings (with \" \\ \n \r \t \uXXXX),
// arrays, objects. Skips whitespace. Throws std::runtime_error on
// malformed input.

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* find(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    int      as_int   (int      f = 0)    const { return type == Number ? (int)num      : f; }
    uint32_t as_u32   (uint32_t f = 0)    const { return type == Number ? (uint32_t)num : f; }
    float    as_float (float    f = 0.f)  const { return type == Number ? (float)num    : f; }
    bool     as_bool  (bool     f = false)const { return type == Bool   ? b             : f; }
    std::string as_string(const std::string& f = "") const {
        return type == String ? s : f;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : i_src(src), i_pos(0) {}
    JsonValue Parse() {
        SkipWs();
        JsonValue v = ParseValue();
        SkipWs();
        if (i_pos != i_src.size()) Fail("trailing garbage");
        return v;
    }
private:
    const std::string& i_src;
    size_t             i_pos;

    [[noreturn]] void Fail(const char* msg) {
        std::ostringstream os;
        os << "JSON parse error at offset " << i_pos << ": " << msg;
        throw std::runtime_error(os.str());
    }
    char Peek() {
        if (i_pos >= i_src.size()) Fail("unexpected EOF");
        return i_src[i_pos];
    }
    char Get() {
        if (i_pos >= i_src.size()) Fail("unexpected EOF");
        return i_src[i_pos++];
    }
    void Expect(char c) {
        if (Peek() != c) Fail("expected character mismatch");
        ++i_pos;
    }
    void SkipWs() {
        while (i_pos < i_src.size()) {
            char c = i_src[i_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_pos;
            else break;
        }
    }

    JsonValue ParseValue() {
        SkipWs();
        char c = Peek();
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') return ParseString();
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') return ParseNull();
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
        Fail("unexpected character");
    }

    JsonValue ParseObject() {
        Expect('{');
        JsonValue v;
        v.type = JsonValue::Object;
        SkipWs();
        if (Peek() == '}') { ++i_pos; return v; }
        while (true) {
            SkipWs();
            JsonValue k = ParseString();
            SkipWs();
            Expect(':');
            JsonValue val = ParseValue();
            v.obj.emplace_back(k.s, std::move(val));
            SkipWs();
            char c = Get();
            if (c == ',') continue;
            if (c == '}') break;
            Fail("expected , or }");
        }
        return v;
    }
    JsonValue ParseArray() {
        Expect('[');
        JsonValue v;
        v.type = JsonValue::Array;
        SkipWs();
        if (Peek() == ']') { ++i_pos; return v; }
        while (true) {
            v.arr.push_back(ParseValue());
            SkipWs();
            char c = Get();
            if (c == ',') continue;
            if (c == ']') break;
            Fail("expected , or ]");
        }
        return v;
    }
    JsonValue ParseString() {
        Expect('"');
        JsonValue v;
        v.type = JsonValue::String;
        while (true) {
            if (i_pos >= i_src.size()) Fail("unterminated string");
            char c = i_src[i_pos++];
            if (c == '"') break;
            if (c == '\\') {
                if (i_pos >= i_src.size()) Fail("trailing backslash");
                char esc = i_src[i_pos++];
                switch (esc) {
                case '"':  v.s += '"';  break;
                case '\\': v.s += '\\'; break;
                case '/':  v.s += '/';  break;
                case 'b':  v.s += '\b'; break;
                case 'f':  v.s += '\f'; break;
                case 'n':  v.s += '\n'; break;
                case 'r':  v.s += '\r'; break;
                case 't':  v.s += '\t'; break;
                case 'u': {
                    if (i_pos + 4 > i_src.size()) Fail("short \\u");
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = i_src[i_pos++];
                        unsigned d = 0;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = 10 + (h - 'a');
                        else if (h >= 'A' && h <= 'F') d = 10 + (h - 'A');
                        else Fail("bad \\u hex");
                        code = (code << 4) | d;
                    }
                    // Encode as UTF-8 (BMP only; surrogate pairs not supported here)
                    if (code < 0x80) v.s += (char)code;
                    else if (code < 0x800) {
                        v.s += (char)(0xC0 | (code >> 6));
                        v.s += (char)(0x80 | (code & 0x3F));
                    } else {
                        v.s += (char)(0xE0 | (code >> 12));
                        v.s += (char)(0x80 | ((code >> 6) & 0x3F));
                        v.s += (char)(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: Fail("bad escape");
                }
            } else {
                v.s += c;
            }
        }
        return v;
    }
    JsonValue ParseBool() {
        JsonValue v;
        v.type = JsonValue::Bool;
        if (i_src.compare(i_pos, 4, "true") == 0) {
            v.b = true;
            i_pos += 4;
        } else if (i_src.compare(i_pos, 5, "false") == 0) {
            v.b = false;
            i_pos += 5;
        } else {
            Fail("expected true/false");
        }
        return v;
    }
    JsonValue ParseNull() {
        if (i_src.compare(i_pos, 4, "null") != 0) Fail("expected null");
        i_pos += 4;
        JsonValue v;
        v.type = JsonValue::Null;
        return v;
    }
    JsonValue ParseNumber() {
        size_t start = i_pos;
        if (Peek() == '-') ++i_pos;
        while (i_pos < i_src.size()) {
            char c = i_src[i_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-')
                ++i_pos;
            else break;
        }
        JsonValue v;
        v.type = JsonValue::Number;
        v.num = std::strtod(i_src.c_str() + start, nullptr);
        return v;
    }
};

// ===== Writer helpers =================================================

std::string Escape(const std::string& s)
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

std::string LayerRefJson(const LayerRef& ref) {
    char buf[80];
    std::snprintf(buf, sizeof(buf),
        "{\"source_id\": %u, \"fnv1a_hash\": %u}",
        ref.source_id, ref.fnv1a_hash);
    return buf;
}

std::string BasenameNoExt(const std::filesystem::path& p) {
    return p.stem().string();
}

std::string SafeForFilename(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out += c;
        } else if (c == ' ') {
            out += '_';
        }
        // else: drop other characters (slashes, colons, etc.)
    }
    if (out.empty()) out = "chase";
    return out;
}

// ===== Chase exporter (single-chase .chase.json) ======================

std::string BuildChaseJson(const Chase& chase, const PanelState& state)
{
    std::string out;
    out += "{\n";
    out += "  \"version\": 1,\n";
    out += "  \"name\": \"" + Escape(chase.name) + "\",\n";
    out += "  \"sort_mode\": " + std::to_string((int)chase.sort_mode) + ",\n";
    out += "  \"sort_reverse\": ";
    out += chase.sort_reverse ? "true" : "false";
    out += ",\n";
    out += "  \"random_seed\": " + std::to_string(chase.random_seed) + ",\n";
    char tbuf[256];
    std::snprintf(tbuf, sizeof(tbuf),
        "  \"timing\": {\n"
        "    \"duration\": %.3f,\n"
        "    \"attack\": %.3f,\n"
        "    \"step_duration\": %.3f,\n"
        "    \"opacity_peak\": %.3f,\n"
        "    \"gamma_peak\": %.3f,\n"
        "    \"gamma_baseline\": %.3f\n"
        "  },\n",
        chase.timing.duration, chase.timing.attack,
        chase.timing.step_duration, chase.timing.opacity_peak,
        chase.timing.gamma_peak, chase.timing.gamma_baseline);
    out += tbuf;
    out += "  \"stages\": [\n";
    for (size_t si = 0; si < chase.stages.size(); ++si) {
        out += "    [";
        const auto& stage = chase.stages[si];
        for (size_t mi = 0; mi < stage.members.size(); ++mi) {
            const LayerRef& m = stage.members[mi];
            const LayerInfo* L = FindLayerByRef(state, m);
            const Source* src = FindSourceById(state, m.source_id);
            std::snprintf(tbuf, sizeof(tbuf),
                "\n      {\"source_id\": %u, \"fnv1a_hash\": %u, "
                "\"source\": \"%s\", \"layer\": \"%s\"}%s",
                m.source_id, m.fnv1a_hash,
                src ? Escape(std::filesystem::path(src->path).filename().string()).c_str() : "",
                L ? Escape(L->display_name).c_str() : "",
                (mi + 1 < stage.members.size()) ? "," : "");
            out += tbuf;
        }
        out += (si + 1 < chase.stages.size()) ? "\n    ],\n" : "\n    ]\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

// ===== Session writer =================================================

std::string BuildSessionJson(const PanelState& state)
{
    std::string out;
    out += "{\n";
    out += "  \"version\": 1,\n";
    // Sized for the longest fmt in BuildSessionJson — the chase
    // timing block expands to ~150 chars at typical float precision.
    char buf[384];
    std::snprintf(buf, sizeof(buf),
        "  \"next_source_id\": %u,\n"
        "  \"next_bind_id\": %u,\n"
        "  \"next_tag_id\": %u,\n"
        "  \"next_chase_id\": %u,\n"
        "  \"project_fps\": %.4f,\n"
        "  \"project_fps_user\": %d,\n"
        "  \"thumb_max_width\": %d,\n",
        state.next_source_id, state.next_bind_id,
        state.next_tag_id, state.next_chase_id,
        state.project_fps.load(),
        state.project_fps_user.load() ? 1 : 0,
        state.thumb_max_width);
    out += buf;

    // Sources (path + id only; layers regenerated on load)
    out += "  \"sources\": [\n";
    for (size_t i = 0; i < state.sources.size(); ++i) {
        const Source& src = state.sources[i];
        std::snprintf(buf, sizeof(buf),
            "    {\"source_id\": %u, \"scan_frame\": %d, "
            "\"animation\": %d, \"path\": \"",
            src.source_id, src.scan_frame, src.animation ? 1 : 0);
        out += buf;
        out += Escape(src.path);
        out += "\"}";
        if (i + 1 < state.sources.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    // Active source
    std::snprintf(buf, sizeof(buf), "  \"active_source_index\": %d,\n",
                  state.active_source_index);
    out += buf;

    // Binds
    out += "  \"binds\": [\n";
    for (size_t i = 0; i < state.binds.size(); ++i) {
        const Bind& b = state.binds[i];
        std::snprintf(buf, sizeof(buf),
            "    {\"bind_id\": %u, \"name\": \"", b.bind_id);
        out += buf;
        out += Escape(b.name);
        std::snprintf(buf, sizeof(buf), "\", \"color\": %u, \"members\": [", b.color);
        out += buf;
        for (size_t mi = 0; mi < b.members.size(); ++mi) {
            out += LayerRefJson(b.members[mi]);
            if (mi + 1 < b.members.size()) out += ", ";
        }
        out += "]}";
        if (i + 1 < state.binds.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    // Tags
    out += "  \"tags\": [\n";
    for (size_t i = 0; i < state.tags.size(); ++i) {
        const Tag& t = state.tags[i];
        std::snprintf(buf, sizeof(buf),
            "    {\"tag_id\": %u, \"name\": \"", t.tag_id);
        out += buf;
        out += Escape(t.name);
        std::snprintf(buf, sizeof(buf), "\", \"color\": %u, \"members\": [", t.color);
        out += buf;
        for (size_t mi = 0; mi < t.members.size(); ++mi) {
            out += LayerRefJson(t.members[mi]);
            if (mi + 1 < t.members.size()) out += ", ";
        }
        out += "]}";
        if (i + 1 < state.tags.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    // Position overrides
    out += "  \"position_overrides\": [\n";
    for (size_t i = 0; i < state.position_overrides.size(); ++i) {
        const PositionOverride& po = state.position_overrides[i];
        std::snprintf(buf, sizeof(buf),
            "    {\"source_id\": %u, \"fnv1a_hash\": %u, "
            "\"cx\": %.4f, \"cy\": %.4f, \"cz\": %.4f}",
            po.layer.source_id, po.layer.fnv1a_hash, po.cx, po.cy, po.cz);
        out += buf;
        if (i + 1 < state.position_overrides.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    // Chases
    out += "  \"chases\": [\n";
    for (size_t ci = 0; ci < state.chases.size(); ++ci) {
        const Chase& c = state.chases[ci];
        out += "    {\n";
        std::snprintf(buf, sizeof(buf),
            "      \"chase_id\": %u,\n"
            "      \"name\": \"", c.chase_id);
        out += buf;
        out += Escape(c.name);
        out += "\",\n";
        std::snprintf(buf, sizeof(buf),
            "      \"sort_mode\": %d, \"sort_reverse\": %s, \"random_seed\": %u,\n"
            "      \"desired_stage_count\": %d, \"symmetric_pairs\": %s, "
            "\"manual_stages\": %s,\n"
            "      \"random_scatter\": %s, \"loop_seconds\": %.3f, "
            "\"scatter_density\": %d, \"loop_multiple\": %d,\n",
            (int)c.sort_mode, c.sort_reverse ? "true" : "false", c.random_seed,
            c.desired_stage_count, c.symmetric_pairs ? "true" : "false",
            c.manual_stages ? "true" : "false",
            c.random_scatter ? "true" : "false", c.loop_seconds,
            c.scatter_density, c.loop_multiple < 1 ? 1 : c.loop_multiple);
        out += buf;
        out += "      \"tag_filter\": [";
        for (size_t ti = 0; ti < c.tag_filter.size(); ++ti) {
            std::snprintf(buf, sizeof(buf), "%u", c.tag_filter[ti]);
            out += buf;
            if (ti + 1 < c.tag_filter.size()) out += ", ";
        }
        out += "],\n";
        std::snprintf(buf, sizeof(buf),
            "      \"timing\": {\"duration\": %.3f, \"attack\": %.3f, "
            "\"step_duration\": %.3f, \"opacity_peak\": %.3f, "
            "\"gamma_peak\": %.3f, \"gamma_baseline\": %.3f},\n",
            c.timing.duration, c.timing.attack, c.timing.step_duration,
            c.timing.opacity_peak, c.timing.gamma_peak, c.timing.gamma_baseline);
        out += buf;
        out += "      \"stages\": [";
        for (size_t si = 0; si < c.stages.size(); ++si) {
            out += "[";
            const auto& stage = c.stages[si];
            for (size_t mi = 0; mi < stage.members.size(); ++mi) {
                out += LayerRefJson(stage.members[mi]);
                if (mi + 1 < stage.members.size()) out += ", ";
            }
            out += "]";
            if (si + 1 < c.stages.size()) out += ", ";
        }
        out += "]\n";
        out += "    }";
        if (ci + 1 < state.chases.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    // Default timing
    std::snprintf(buf, sizeof(buf),
        "  \"default_timing\": {\"duration\": %.3f, \"attack\": %.3f, "
        "\"step_duration\": %.3f, \"opacity_peak\": %.3f, "
        "\"gamma_peak\": %.3f, \"gamma_baseline\": %.3f}\n",
        state.default_timing.duration, state.default_timing.attack,
        state.default_timing.step_duration, state.default_timing.opacity_peak,
        state.default_timing.gamma_peak, state.default_timing.gamma_baseline);
    out += buf;

    out += "}\n";
    return out;
}

// ===== Loader helpers =================================================

LayerRef ReadLayerRef(const JsonValue& v) {
    LayerRef r;
    if (const JsonValue* sid = v.find("source_id")) r.source_id = sid->as_u32();
    if (const JsonValue* h   = v.find("fnv1a_hash")) r.fnv1a_hash = h->as_u32();
    return r;
}

ChaseTiming ReadTiming(const JsonValue& v) {
    ChaseTiming t;
    if (const JsonValue* x = v.find("duration"))        t.duration        = x->as_float();
    if (const JsonValue* x = v.find("attack"))          t.attack          = x->as_float();
    if (const JsonValue* x = v.find("step_duration"))   t.step_duration   = x->as_float();
    if (const JsonValue* x = v.find("opacity_peak"))    t.opacity_peak    = x->as_float();
    if (const JsonValue* x = v.find("gamma_peak"))      t.gamma_peak      = x->as_float();
    if (const JsonValue* x = v.find("gamma_baseline")) t.gamma_baseline = x->as_float();
    return t;
}

} // namespace

bool WriteSession(PanelState* state, const std::string& path)
{
    if (!state) return false;

    std::string session_json;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        session_json = BuildSessionJson(*state);
    }

    // Write session JSON. Per-chase .chase.json sidecars are no
    // longer emitted — the chase output path is now a direct AEGP
    // comp-builder (under construction). The session JSON itself
    // still contains every chase's data so authoring round-trips.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = "Could not open session for writing: " + path;
        return false;
    }
    out << session_json;
    if (!out.good()) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = "Write failed mid-stream.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->session_save_path = path;
        state->last_status = "Session saved.";
        state->last_error.clear();
    }
    return true;
}

bool LoadSession(PanelState* state, const std::string& path)
{
    if (!state) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = "Could not open session for reading: " + path;
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string text = buf.str();

    JsonValue root;
    try {
        JsonParser parser(text);
        root = parser.Parse();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = std::string("Bad session JSON: ") + e.what();
        return false;
    }

    if (root.type != JsonValue::Object) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = "Session JSON root is not an object.";
        return false;
    }

    // Capture source paths + ids (+ saved scan frame) before we reset
    // state so we can start scans outside the lock.
    struct RescanReq { uint32_t id; std::string path; int frame; int anim; };
    std::vector<RescanReq> sources_to_rescan;

    {
        std::lock_guard<std::mutex> lk(state->mu);
        // Clear current state.
        state->sources.clear();
        state->active_source_index = -1;
        state->binds.clear();
        state->tags.clear();
        state->position_overrides.clear();
        state->chases.clear();
        state->active_chase_index = -1;
        state->chase_in_wizard = false;

        if (const JsonValue* v = root.find("next_source_id")) state->next_source_id = v->as_u32(1);
        if (const JsonValue* v = root.find("next_bind_id"))   state->next_bind_id   = v->as_u32(1);
        if (const JsonValue* v = root.find("next_tag_id"))    state->next_tag_id    = v->as_u32(1);
        if (const JsonValue* v = root.find("next_chase_id")) state->next_chase_id = v->as_u32(1);
        if (const JsonValue* v = root.find("project_fps")) {
            float f = v->as_float(24.f);
            state->project_fps.store(f < 1.f ? 1.f : (f > 240.f ? 240.f : f));
        }
        if (const JsonValue* v = root.find("project_fps_user"))
            state->project_fps_user.store(v->as_int(0) != 0);
        if (const JsonValue* v = root.find("thumb_max_width")) {
            int tw = v->as_int(384);
            state->thumb_max_width = tw < 64 ? 64 : (tw > 1024 ? 1024 : tw);
        }
        if (const JsonValue* v = root.find("active_source_index"))
            state->active_source_index = v->as_int(-1);

        // Sources (paths only; layers come from rescans).
        if (const JsonValue* arr = root.find("sources")) {
            for (const auto& e : arr->arr) {
                uint32_t sid = 0;
                std::string p;
                int frame = 0;
                int anim = -1;   // -1 = not stored, keep scan auto-default
                if (const JsonValue* x = e.find("source_id"))  sid   = x->as_u32();
                if (const JsonValue* x = e.find("path"))       p     = x->as_string();
                if (const JsonValue* x = e.find("scan_frame")) frame = x->as_int(0);
                if (const JsonValue* x = e.find("animation"))  anim  = x->as_int(-1);
                if (sid != 0 && !p.empty())
                    sources_to_rescan.push_back({sid, p, frame, anim});
            }
        }

        // Binds
        if (const JsonValue* arr = root.find("binds")) {
            for (const auto& e : arr->arr) {
                Bind b;
                if (const JsonValue* x = e.find("bind_id")) b.bind_id = x->as_u32();
                if (const JsonValue* x = e.find("name"))    b.name    = x->as_string();
                if (const JsonValue* x = e.find("color"))   b.color   = x->as_u32();
                if (const JsonValue* x = e.find("members")) {
                    for (const auto& m : x->arr) b.members.push_back(ReadLayerRef(m));
                }
                state->binds.push_back(std::move(b));
            }
        }
        // Tags
        if (const JsonValue* arr = root.find("tags")) {
            for (const auto& e : arr->arr) {
                Tag t;
                if (const JsonValue* x = e.find("tag_id")) t.tag_id = x->as_u32();
                if (const JsonValue* x = e.find("name"))   t.name   = x->as_string();
                if (const JsonValue* x = e.find("color"))  t.color  = x->as_u32();
                if (const JsonValue* x = e.find("members")) {
                    for (const auto& m : x->arr) t.members.push_back(ReadLayerRef(m));
                }
                state->tags.push_back(std::move(t));
            }
        }
        // Position overrides
        if (const JsonValue* arr = root.find("position_overrides")) {
            for (const auto& e : arr->arr) {
                PositionOverride po;
                po.layer = ReadLayerRef(e);
                if (const JsonValue* x = e.find("cx")) po.cx = x->as_float();
                if (const JsonValue* x = e.find("cy")) po.cy = x->as_float();
                if (const JsonValue* x = e.find("cz")) po.cz = x->as_float();
                state->position_overrides.push_back(po);
            }
        }
        // Chases
        if (const JsonValue* arr = root.find("chases")) {
            for (const auto& e : arr->arr) {
                Chase c;
                if (const JsonValue* x = e.find("chase_id"))     c.chase_id     = x->as_u32();
                if (const JsonValue* x = e.find("name"))         c.name         = x->as_string();
                if (const JsonValue* x = e.find("sort_mode"))    c.sort_mode    = (SortMode)x->as_int();
                if (const JsonValue* x = e.find("sort_reverse")) c.sort_reverse = x->as_bool();
                if (const JsonValue* x = e.find("random_seed"))  c.random_seed  = x->as_u32();
                if (const JsonValue* x = e.find("desired_stage_count")) c.desired_stage_count = x->as_int(0);
                if (const JsonValue* x = e.find("symmetric_pairs")) c.symmetric_pairs = x->as_bool();
                if (const JsonValue* x = e.find("manual_stages"))   c.manual_stages   = x->as_bool();
                if (const JsonValue* x = e.find("random_scatter"))  c.random_scatter  = x->as_bool();
                if (const JsonValue* x = e.find("loop_seconds"))    c.loop_seconds    = x->as_float(10.0f);
                if (const JsonValue* x = e.find("scatter_density")) c.scatter_density = x->as_int(5);
                if (const JsonValue* x = e.find("loop_multiple"))   c.loop_multiple   = x->as_int(1);
                if (const JsonValue* x = e.find("tag_filter")) {
                    for (const auto& t : x->arr) c.tag_filter.push_back(t.as_u32());
                }
                if (const JsonValue* x = e.find("timing")) c.timing = ReadTiming(*x);
                if (const JsonValue* x = e.find("stages")) {
                    for (const auto& s : x->arr) {
                        ChaseStage cs;
                        for (const auto& m : s.arr) cs.members.push_back(ReadLayerRef(m));
                        c.stages.push_back(std::move(cs));
                    }
                }
                state->chases.push_back(std::move(c));
            }
        }
        if (const JsonValue* v = root.find("default_timing")) {
            state->default_timing = ReadTiming(*v);
        }

        state->session_save_path = path;
        state->last_status = "Session loaded.";
        state->last_error.clear();
    }

    // Kick off scans for each saved source (outside the lock — each
    // StartScan grabs it briefly). Each scan appends a source with
    // the saved ID so saved LayerRefs keep resolving once scans
    // complete.
    for (const auto& kv : sources_to_rescan) {
        exr_scan::StartScan(kv.path, state, /*append=*/true,
                            /*source_id=*/kv.id, /*frame_index=*/kv.frame);
        // Block until current scan finishes — StartScan refuses re-entry
        // while scanning is true. We poll briefly to serialize.
        while (state->scanning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // Re-apply the saved animation override (the scan resets it to
        // the frame_count auto-default; the user's choice must win).
        if (kv.anim >= 0) {
            std::lock_guard<std::mutex> lk(state->mu);
            if (Source* s = FindSourceById(*state, kv.id))
                s->animation = (kv.anim != 0);
        }
    }
    return true;
}

} // namespace session_io
