#include "session_io.h"

#include "panel_state.h"
#include "chase_gen.h"
#include "exr_scan.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

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
        "  \"thumb_max_width\": %d,\n"
        "  \"dedupe_by_name\": %d,\n"
        "  \"export_split_groups\": %d,\n",
        state.next_source_id, state.next_bind_id,
        state.next_tag_id, state.next_chase_id,
        state.project_fps.load(),
        state.project_fps_user.load() ? 1 : 0,
        state.thumb_max_width,
        state.dedupe_by_name ? 1 : 0,
        state.export_split_groups ? 1 : 0);
    out += buf;

    // The comp-name prefix the artist last exported with
    // ("USC_CatTowers"), so the next export dialog opens prefilled.
    out += "  \"export_prefix\": \"" + Escape(state.export_prefix) + "\",\n";

    // Per-name dedupe winner overrides — pinned by the user via
    // the Staging tab's right-click. Keyed by display_name.
    out += "  \"preferred_source_per_name\": {";
    {
        bool first = true;
        for (const auto& kv : state.preferred_source_per_name) {
            if (!first) out += ", ";
            first = false;
            out += "\"";
            out += Escape(kv.first);
            std::snprintf(buf, sizeof(buf), "\": %u", kv.second);
            out += buf;
        }
    }
    out += "},\n";

    // Sources (path + id only; layers regenerated on load)
    out += "  \"sources\": [\n";
    for (size_t i = 0; i < state.sources.size(); ++i) {
        const Source& src = state.sources[i];
        std::snprintf(buf, sizeof(buf),
            "    {\"source_id\": %u, \"scan_frame\": %d, "
            "\"animation\": %d, \"is_movie\": %d, \"path\": \"",
            src.source_id, src.scan_frame, src.animation ? 1 : 0,
            src.is_movie ? 1 : 0);
        out += buf;
        out += Escape(src.path);
        out += "\"}";
        if (i + 1 < state.sources.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    // Per-layer Excluded list. Layers themselves are regenerated by
    // scan on load (we don't store them), so to persist the user's
    // Staging-tab uncheck choices we save only the EXCLUDED ones as
    // (source_id, display_name) pairs. After each source's load-time
    // scan finishes we walk its layers and clear `included` for any
    // matching entry. Display_name is the right key because that's
    // what's stable across rescans + matches our (source_id, name)
    // dispatch elsewhere.
    out += "  \"excluded_layers\": [";
    {
        bool first = true;
        for (const Source& src : state.sources) {
            for (const LayerInfo& L : src.layers) {
                if (L.included) continue;
                if (!first) out += ", ";
                first = false;
                std::snprintf(buf, sizeof(buf),
                    "{\"source_id\": %u, \"name\": \"", src.source_id);
                out += buf;
                out += Escape(L.display_name);
                out += "\"}";
            }
        }
    }
    out += "],\n";

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
            "\"scatter_density\": %.4f, \"loop_multiple\": %d, "
            "\"loop_cycles\": %d, \"loop_offset\": %d, \"loops\": %s,\n"
            "      \"tag_balanced_chunks\": %s,\n",
            (int)c.sort_mode, c.sort_reverse ? "true" : "false", c.random_seed,
            c.desired_stage_count, c.symmetric_pairs ? "true" : "false",
            c.manual_stages ? "true" : "false",
            c.random_scatter ? "true" : "false", c.loop_seconds,
            c.scatter_density,
            c.loop_multiple < 1 ? 1 : c.loop_multiple,
            c.loop_cycles   < 1 ? 1 : c.loop_cycles,
            c.loop_offset   < 0 ? 0 : c.loop_offset,
            c.loops ? "true" : "false",
            c.tag_balanced_chunks ? "true" : "false");
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
            "\"hold\": %.3f, "
            "\"step_duration\": %.3f, \"opacity_peak\": %.3f, "
            "\"opacity_floor\": %.3f, "
            "\"gamma_peak\": %.3f, \"gamma_baseline\": %.3f},\n",
            c.timing.duration, c.timing.attack, c.timing.hold,
            c.timing.step_duration,
            c.timing.opacity_peak, c.timing.opacity_floor,
            c.timing.gamma_peak, c.timing.gamma_baseline);
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
        "\"hold\": %.3f, "
        "\"step_duration\": %.3f, \"opacity_peak\": %.3f, "
        "\"opacity_floor\": %.3f, "
        "\"gamma_peak\": %.3f, \"gamma_baseline\": %.3f}\n",
        state.default_timing.duration, state.default_timing.attack,
        state.default_timing.hold,
        state.default_timing.step_duration, state.default_timing.opacity_peak,
        state.default_timing.opacity_floor,
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
    if (const JsonValue* x = v.find("hold"))            t.hold            = x->as_float();
    if (const JsonValue* x = v.find("step_duration"))   t.step_duration   = x->as_float();
    if (const JsonValue* x = v.find("opacity_peak"))    t.opacity_peak    = x->as_float();
    if (const JsonValue* x = v.find("opacity_floor"))   t.opacity_floor   = x->as_float();
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

    // Write session JSON. The chase output path is the in-process
    // AEGP comp-builder; the session JSON carries every chase's data
    // so authoring round-trips.
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

std::string SerializeSession(PanelState* state)
{
    if (!state) return {};
    std::lock_guard<std::mutex> lk(state->mu);
    return BuildSessionJson(*state);
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
    return LoadSessionText(state, buf.str(), path);
}

// The whole of loading, minus where the bytes came from. `path` is
// remembered as the save default and may be empty — it is when the
// session came out of the AE project rather than off disk.
bool LoadSessionText(PanelState* state, const std::string& text,
                     const std::string& path)
{
    if (!state) return false;

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
    // Saved per-layer Excluded list — applied after each source's
    // scan finishes (the scanner sets included=true by default; we
    // mark these false to restore the user's Staging unchecks).
    struct ExcludedRef { uint32_t source_id; std::string name; };
    std::vector<ExcludedRef> excluded_layers;
    // Whether the file carried the field at all. An empty list means
    // "nothing is excluded"; a missing field means "this session
    // predates the list" — those must not be treated the same.
    bool saw_excluded_list = false;

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
        if (const JsonValue* v = root.find("dedupe_by_name"))
            state->dedupe_by_name = v->as_int(0) != 0;
        if (const JsonValue* v = root.find("export_split_groups"))
            state->export_split_groups = v->as_int(0) != 0;
        if (const JsonValue* v = root.find("export_prefix"))
            state->export_prefix = v->as_string();
        if (const JsonValue* v = root.find("preferred_source_per_name")) {
            // Object: display_name -> source_id. Keys are the
            // user's pinned dedupe winners (right-click "Use this
            // source for..." in Staging).
            state->preferred_source_per_name.clear();
            for (const auto& kv : v->obj) {
                state->preferred_source_per_name[kv.first] =
                    kv.second.as_u32();
            }
        }
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
        // Excluded layers (per-source uncheck state, applied after
        // each rescan finishes — the scanner doesn't know about
        // these). Older sessions without this field simply load
        // everything as included.
        if (const JsonValue* arr = root.find("excluded_layers")) {
            saw_excluded_list = true;
            for (const auto& e : arr->arr) {
                ExcludedRef er;
                if (const JsonValue* x = e.find("source_id"))
                    er.source_id = x->as_u32();
                if (const JsonValue* x = e.find("name"))
                    er.name = x->as_string();
                if (er.source_id != 0 && !er.name.empty())
                    excluded_layers.push_back(std::move(er));
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
                // Absent in sessions written before 3 Step looped.
                if (const JsonValue* x = e.find("loops"))           c.loops           = x->as_bool();
                if (const JsonValue* x = e.find("tag_balanced_chunks")) c.tag_balanced_chunks = x->as_bool();
                if (const JsonValue* x = e.find("loop_seconds"))    c.loop_seconds    = x->as_float(10.0f);
                if (const JsonValue* x = e.find("scatter_density")) c.scatter_density = x->as_float(5.f);
                if (const JsonValue* x = e.find("loop_multiple"))   c.loop_multiple   = x->as_int(1);
                if (const JsonValue* x = e.find("loop_cycles"))     c.loop_cycles     = x->as_int(1);
                if (const JsonValue* x = e.find("loop_offset"))     c.loop_offset     = x->as_int(0);
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

    // Kick off a (re)load for each saved source (outside the lock —
    // each grabs it briefly). Each gets the saved ID so saved LayerRefs
    // keep resolving. AddSourcePath routes by extension: EXR/sequence
    // sources re-scan on a worker thread; movie sources re-create a
    // placeholder (same FNV-of-stem hash, so refs resolve immediately)
    // and re-queue AE-render analysis for metrics.
    for (const auto& kv : sources_to_rescan) {
        exr_scan::AddSourcePath(kv.path, state, /*append=*/true,
                                /*source_id=*/kv.id, /*frame_index=*/kv.frame);
        // Block until any worker scan finishes — StartScan refuses
        // re-entry while scanning is true. Movie sources don't set this
        // flag (they load synchronously), so this is a no-op for them.
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
        // Re-apply the Staging-tab tick state for this source. The
        // saved list is AUTHORITATIVE: a layer named in it is
        // unchecked, and anything else is checked. Restoring only the
        // unchecks would be wrong for single-light sources, where the
        // scanner excludes scenery names (World.mov and friends) by
        // default — a user who deliberately ticked one back on would
        // find it unticked again on every reload.
        //
        // Sessions saved before the excluded list existed carry none,
        // so they load fully included, as they always have.
        if (saw_excluded_list) {
            std::lock_guard<std::mutex> lk(state->mu);
            Source* s = FindSourceById(*state, kv.id);
            if (s) {
                for (LayerInfo& L : s->layers) {
                    bool excluded = false;
                    for (const ExcludedRef& er : excluded_layers) {
                        if (er.source_id == kv.id && er.name == L.display_name) {
                            excluded = true;
                            break;
                        }
                    }
                    L.included = !excluded;
                }
            }
        }
    }

    // Drop references to sources this session file never listed. Those
    // accumulate when a source is removed and re-added (the new one
    // mints a fresh id, and the old refs were never cleaned up); they
    // can never resolve, and they inflate every count the UI shows.
    //
    // Keyed on the ids the FILE listed, NOT on what actually loaded: a
    // failed scan adds no Source at all, so judging by state.sources
    // would cost the user their tags whenever an EXR had moved.
    {
        std::vector<uint32_t> from_file;
        from_file.reserve(sources_to_rescan.size());
        for (const auto& kv : sources_to_rescan) from_file.push_back(kv.id);

        std::lock_guard<std::mutex> lk(state->mu);
        const int dropped = chase_gen::PruneOrphanedRefs(*state, from_file);
        if (dropped > 0) {
            char buf[144];
            std::snprintf(buf, sizeof(buf),
                          "Cleaned up %d stale reference%s to sources that "
                          "are no longer in this session.", dropped,
                          dropped == 1 ? "" : "s");
            state->last_status = buf;
        }
    }
    return true;
}

// ===== User preferences ===============================================
//
// Deliberately NOT part of a session. A session describes a scene; this
// describes how one person likes to look at scenes, and importing a
// fresh scene must not reset it. Best-effort at every step: a missing
// directory, an unwritable path or a mangled file all mean "use the
// defaults", never an error the artist has to acknowledge.

namespace {

std::string PrefsPath()
{
    // An override so the tests can round-trip prefs without writing
    // over the real user's file.
    if (const char* over = std::getenv("CHASEMAKER_PREFS_DIR")) {
        if (*over) return std::string(over) + "/prefs.json";
    }
    // No function-local static holding the base: this is called from
    // the plugin constructor AND from the UI thread, and a static that
    // gets REWRITTEN on each call is a data race waiting to happen.
    // Build the whole path locally and return it by value.
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        if (*appdata) {
            return std::string(appdata) + "/ChaseMaker/prefs.json";
        }
    }
#else
    // macOS: ~/Library/Application Support, the conventional home for
    // per-user application state. AE plugins are not sandboxed, so this
    // is writable. Untested on Mac — see the Mac catch-up notes.
    if (const char* home = std::getenv("HOME")) {
        if (*home) {
            return std::string(home) +
                   "/Library/Application Support/ChaseMaker/prefs.json";
        }
    }
#endif
    return {};
}

// mkdir -p for the directories we need, without pulling <filesystem>
// into this translation unit's error handling. Walks the whole path
// rather than making only the last component: on a machine where
// ~/Library/Application Support somehow doesn't exist, a single mkdir
// of the leaf would fail and the prefs would silently never save.
// Existing directories are expected to fail with EEXIST; ignore that.
void EnsurePrefsDir(const std::string& file_path)
{
    const size_t last = file_path.find_last_of("/\\");
    if (last == std::string::npos) return;

    for (size_t i = 1; i <= last; ++i) {
        const char c = file_path[i];
        if (i != last && c != '/' && c != '\\') continue;
        const std::string dir = file_path.substr(0, i);
        if (dir.empty()) continue;
#ifdef _WIN32
        // Skip a bare drive prefix ("C:").
        if (dir.size() == 2 && dir[1] == ':') continue;
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0755);
#endif
    }
}

} // namespace

void LoadPrefs(PanelState* state)
{
    if (!state) return;
    const std::string path = PrefsPath();
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return;

    try {
        JsonParser parser(text);
        const JsonValue root = parser.Parse();
        if (const JsonValue* v = root.find("sheet_cols")) {
            const int cols = v->as_int(state->sheet_cols);
            // Clamp rather than trust the file: the Zoom control only
            // offers 1..6 columns and a wild value would draw nothing.
            state->sheet_cols = (cols < 1) ? 1 : (cols > 6 ? 6 : cols);
        }
    } catch (const std::exception&) {
        // Corrupt prefs are not worth telling anyone about.
    }
}

void SavePrefs(const PanelState* state)
{
    if (!state) return;
    const std::string path = PrefsPath();
    if (path.empty()) return;
    EnsurePrefsDir(path);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\n  \"sheet_cols\": %d\n}\n", state->sheet_cols);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << buf;
}

// Back to a freshly-opened panel: no sources, no chases, no tags, no
// undo history, nothing cached from the last scene. Deliberately does
// NOT reset the per-user preferences (contact-sheet zoom) or the
// project FPS pin — those are how this person works, not what this
// scene is.
//
// Clearing sources drops every LayerInfo, and with it the texture ids
// the renderer handed out. That is exactly what LoadSession already
// does, and `scan_generation` is what tells the renderer its thumbnail
// cache is stale — bump it so a reopened panel doesn't draw the old
// scene's textures.
void NewSession(PanelState* state)
{
    if (!state) return;
    std::lock_guard<std::mutex> lk(state->mu);

    state->sources.clear();
    state->active_source_index = -1;
    state->binds.clear();
    state->tags.clear();
    state->position_overrides.clear();
    state->chases.clear();
    state->active_chase_index = -1;
    state->chase_in_wizard = false;
    state->chase_pending_close_index = -1;

    state->next_source_id = 1;
    state->next_bind_id   = 1;
    state->next_tag_id    = 1;
    state->next_chase_id  = 1;

    state->selected_hashes.clear();
    state->preferred_source_per_name.clear();
    state->dedupe_by_name = false;
    state->exclude_filter[0] = 0;

    // Review tab: every cached playhead, pause and grid measurement
    // belongs to chases that no longer exist.
    state->sheet_playheads.clear();
    state->sheet_paused_ids.clear();
    state->sheet_unchecked.clear();
    state->sheet_all_paused = false;
    state->sheet_sig = 0;
    state->sheet_t_key = -1;
    state->sheet_count = 0;
    state->sheet_grid_cols = 0;
    state->sheet_grid_rows = 0;
    state->sheet_cell_w = 0;
    state->sheet_cell_h = 0;
    state->sheet_px_w = 0;
    state->sheet_px_h = 0;

    state->chase_composite_rgba.clear();
    state->chase_composite_w = 0;
    state->chase_composite_h = 0;
    state->chase_composite_dirty = true;

    state->build_queue.clear();
    state->want_build_queue = false;

    // Undo history from a scene that is gone would restore that scene.
    state->undo_stack.clear();
    state->redo_stack.clear();
    state->has_last_stable = false;

    state->session_save_path.clear();
    state->last_error.clear();
    state->last_status = "New session.";

    // Invalidate any in-flight scan's thumbnails.
    state->scan_generation.fetch_add(1);
}

} // namespace session_io
