#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

constexpr uint32_t AFS_MAGIC_00   = 0x00534641; // "AFS\0"
constexpr uint32_t AFS_MAGIC_20   = 0x20534641; // "AFS "
constexpr uint32_t HEADER_SIZE    = 8;
constexpr uint32_t ENTRY_INFO_SZ  = 8;
constexpr uint32_t ATTR_INFO_SZ   = 8;
constexpr uint32_t ATTR_ELEM_SZ   = 0x30; // 48
constexpr uint32_t MAX_NAME_LEN   = 32;
constexpr uint32_t MIN_ALIGNMENT  = 0x800;
constexpr uint32_t DATA_ALIGNMENT = 0x800;

// ===================================================================
// Little-endian I/O
// ===================================================================

static uint32_t read_u32_le(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return p[0] | (p[1] << 8);
}

template<typename T>
static T read_le(std::ifstream &f) {
    T v;
    f.read(reinterpret_cast<char *>(&v), sizeof(v));
    return v;
}

static void write_u32_le(std::ostream &out, uint32_t v) {
    out.put(static_cast<unsigned char>(v & 0xFF));
    out.put(static_cast<unsigned char>((v >> 8) & 0xFF));
    out.put(static_cast<unsigned char>((v >> 16) & 0xFF));
    out.put(static_cast<unsigned char>((v >> 24) & 0xFF));
}

static void write_u16_le(std::ostream &out, uint16_t v) {
    out.put(static_cast<unsigned char>(v & 0xFF));
    out.put(static_cast<unsigned char>((v >> 8) & 0xFF));
}

static std::string strip_trailing_sep(const std::string &path) {
    if (path.empty()) return path;
    std::string s = path;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

static uint32_t pad(uint32_t v, uint32_t align) {
    uint32_t mod = v % align;
    return mod ? v + (align - mod) : v;
}

static void fill_zeroes(std::ostream &out, uint32_t count) {
    std::vector<char> z(count, 0);
    out.write(z.data(), count);
}

// ===================================================================
// Entry / archive descriptors
// ===================================================================

struct SourceEntry {
    std::string file_name;   // name on disk (e.g. "hello.txt")
    std::string entry_name;  // 32-char name inside AFS
    uint32_t custom_data = 0;
    bool is_null = false;
};

struct EntryInfo {
    uint32_t offset = 0;
    uint32_t size   = 0;
    std::string name;
    uint16_t year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    uint32_t custom_data = 0;
    bool is_null = false;
};

struct AFSArchive {
    std::string magic_label;
    uint32_t entry_count = 0;
    uint32_t entry_block_alignment = MIN_ALIGNMENT;
    std::string attr_info_label;
    bool has_attributes = false;
    std::vector<EntryInfo> entries;
    std::string file_path;
};

// ===================================================================
// Minimal JSON parser for AFS metadata (zero dependencies)
// ===================================================================

enum class JType { Null, Bool, Number, String, Array, Object };

struct JValue {
    JType type = JType::Null;
    bool    b = false;
    double  n = 0;
    std::string s;
    std::vector<JValue> arr;
    std::map<std::string, JValue> obj;

    bool is_null()   const { return type == JType::Null; }
    bool is_bool()   const { return type == JType::Bool; }
    bool is_number() const { return type == JType::Number; }
    bool is_string() const { return type == JType::String; }
    bool is_array()  const { return type == JType::Array; }
    bool is_object() const { return type == JType::Object; }

    int    as_int()    const { return static_cast<int>(n); }
    bool   as_bool()   const { return b; }
    const std::string& as_string() const { return s; }

    const JValue& operator[](const std::string &key) const {
        static const JValue nul;
        auto it = obj.find(key);
        return it != obj.end() ? it->second : nul;
    }

    const JValue& operator[](size_t i) const {
        static const JValue nul;
        return i < arr.size() ? arr[i] : nul;
    }

    size_t size() const { return arr.size(); }

    // Typed accessors with defaults (for metadata fields)
    const std::string& str(const std::string &key, const std::string &def = {}) const {
        auto &v = (*this)[key];
        return v.is_string() ? v.s : def;
    }
    int num(const std::string &key, int def = 0) const {
        auto &v = (*this)[key];
        return v.is_number() ? v.as_int() : def;
    }
    bool flag(const std::string &key, bool def = false) const {
        auto &v = (*this)[key];
        return v.is_bool() ? v.b : def;
    }
};

struct JParser {
    std::istream *in = nullptr;

    char peek() { return static_cast<char>(in->peek()); }
    char next() { return static_cast<char>(in->get()); }
    bool done() { return in->eof() || in->fail(); }

    void skip_ws() {
        while (!done()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') next();
            else break;
        }
    }

    JValue value() {
        skip_ws();
        if (done()) return {};
        char c = peek();
        if (c == '"') return string_val();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == 't' || c == 'f') return boolean();
        if (c == 'n') return null_val();
        return number();
    }

    JValue string_val() {
        JValue v; v.type = JType::String;
        next(); // opening "
        while (!done()) {
            char c = next();
            if (c == '"') break;
            if (c == '\\') {
                switch (next()) {
                    case '"':  v.s += '"';  break;
                    case '\\': v.s += '\\'; break;
                    case '/':  v.s += '/';  break;
                    case 'n':  v.s += '\n'; break;
                    case 't':  v.s += '\t'; break;
                    case 'r':  v.s += '\r'; break;
                    default: break;
                }
            } else {
                v.s += c;
            }
        }
        return v;
    }

    JValue number() {
        JValue v; v.type = JType::Number;
        std::string num;
        while (!done()) {
            char c = peek();
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                num += next();
            else break;
        }
        v.n = std::stod(num);
        return v;
    }

    JValue boolean() {
        JValue v; v.type = JType::Bool;
        if (peek() == 't') { next(); next(); next(); next(); v.b = true; }
        else               { next(); next(); next(); next(); next(); v.b = false; }
        return v;
    }

    JValue null_val() {
        next(); next(); next(); next(); // null
        return {};
    }

    JValue object() {
        JValue v; v.type = JType::Object;
        next(); // {
        skip_ws();
        if (peek() == '}') { next(); return v; }
        while (true) {
            skip_ws();
            JValue key = string_val();
            skip_ws();
            if (peek() == ':') next();
            v.obj[key.s] = value();
            skip_ws();
            if (peek() == '}') { next(); break; }
            if (peek() == ',') next();
        }
        return v;
    }

    JValue array() {
        JValue v; v.type = JType::Array;
        next(); // [
        skip_ws();
        if (peek() == ']') { next(); return v; }
        while (true) {
            v.arr.push_back(value());
            skip_ws();
            if (peek() == ']') { next(); break; }
            if (peek() == ',') next();
        }
        return v;
    }
};

static JValue parse_json(std::istream &in) {
    JParser p; p.in = &in;
    return p.value();
}

// ===================================================================
// Sanitize a name for the filesystem
// ===================================================================

static std::string null_terminated(const uint8_t *buf, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && buf[len] != 0) ++len;
    return std::string(reinterpret_cast<const char *>(buf), len);
}

static std::string sanitize_name(const std::string &raw) {
    if (raw.empty()) return "_NO_NAME";
    std::string out;
    for (char c : raw) {
        if (c == '\0') break;
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            continue;
        out.push_back(c);
    }
    if (out.empty()) return "_NO_NAME";
    return out;
}

// ===================================================================
// Load AFS archive from file
// ===================================================================

static bool load_afs(const std::string &path, AFSArchive &afs) {
    afs.file_path = path;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Error: cannot open file \"" << path << "\"\n";
        return false;
    }

    uint32_t magic = read_le<uint32_t>(f);
    if (magic == AFS_MAGIC_00) {
        afs.magic_label = "AFS_00";
    } else if (magic == AFS_MAGIC_20) {
        afs.magic_label = "AFS_20";
    } else {
        std::cerr << "Error: not a valid AFS file (bad magic: 0x"
                  << std::hex << magic << std::dec << ")\n";
        return false;
    }

    afs.entry_count = read_le<uint32_t>(f);
    if (afs.entry_count == 0) {
        std::cerr << "Warning: AFS file has 0 entries.\n";
        return true;
    }

    afs.entries.resize(afs.entry_count);
    uint32_t first_offset = 0, last_end = 0;
    for (uint32_t i = 0; i < afs.entry_count; ++i) {
        auto &e = afs.entries[i];
        e.offset = read_le<uint32_t>(f);
        e.size   = read_le<uint32_t>(f);
        e.is_null = (e.offset == 0 && e.size == 0);
        if (!e.is_null) {
            if (first_offset == 0) first_offset = e.offset;
            last_end = e.offset + e.size;
        }
    }

    uint32_t end_of_info = HEADER_SIZE + afs.entry_count * ENTRY_INFO_SZ + ATTR_INFO_SZ;
    uint32_t alignment = MIN_ALIGNMENT;
    while (end_of_info + alignment < first_offset) alignment <<= 1;
    afs.entry_block_alignment = alignment;

    afs.has_attributes = false;
    afs.attr_info_label = "NoAttributes";

    f.seekg(end_of_info - ATTR_INFO_SZ);
    uint32_t attr_offset = read_le<uint32_t>(f);
    uint32_t attr_size   = read_le<uint32_t>(f);
    uint64_t file_size   = fs::file_size(path);

    // Try to validate the TOC. Standard is 48-byte per entry; some in-house
    // studio pipelines stripped the 16-byte metadata, producing 32-byte entries.
    // The strides are mathematically exclusive: 48N / 32 ≠ M for any N, so
    // there's no ambiguity — checking 48 first always picks the right one.
    uint32_t toc_stride = 0;
    auto try_validate_toc = [&](uint32_t stride) -> bool {
        if (attr_offset == 0 || attr_size == 0) return false;
        if (attr_size % stride != 0) return false;
        if (attr_size / stride != afs.entry_count) return false;
        if (attr_size > file_size - last_end) return false;
        if (attr_offset < last_end) return false;
        if (attr_offset > file_size - attr_size) return false;
        return true;
    };

    auto locate_toc = [&]() -> bool {
        if (try_validate_toc(ATTR_ELEM_SZ)) { toc_stride = ATTR_ELEM_SZ; return true; }
        if (try_validate_toc(32))           { toc_stride = 32; return true; }
        return false;
    };

    if (locate_toc()) {
        afs.has_attributes = true;
        afs.attr_info_label = "InfoAtBeginning";
    } else {
        f.seekg(first_offset - ATTR_INFO_SZ);
        attr_offset = read_le<uint32_t>(f);
        attr_size   = read_le<uint32_t>(f);
        if (locate_toc()) {
            afs.has_attributes = true;
            afs.attr_info_label = "InfoAtEnd";
        }
    }

    if (afs.has_attributes) {
        f.seekg(attr_offset);
        std::vector<uint8_t> attr_buf(attr_size);
        f.read(reinterpret_cast<char *>(attr_buf.data()), attr_size);

        const uint8_t *p = attr_buf.data();
        for (uint32_t i = 0; i < afs.entry_count; ++i) {
            auto &e = afs.entries[i];
            if (!e.is_null) {
                e.name = null_terminated(p, MAX_NAME_LEN);
                if (toc_stride == ATTR_ELEM_SZ) {
                    e.year  = read_u16_le(p + 32);
                    e.month = read_u16_le(p + 34);
                    e.day   = read_u16_le(p + 36);
                    e.hour  = read_u16_le(p + 38);
                    e.min   = read_u16_le(p + 40);
                    e.sec   = read_u16_le(p + 42);
                    e.custom_data = read_u32_le(p + 44);
                }
            }
            p += toc_stride;
        }
    } else {
        for (uint32_t i = 0; i < afs.entry_count; ++i) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%08u", i);
            afs.entries[i].name = buf;
        }
    }

    return true;
}

// ===================================================================
// Magic-based extension detection for Dreamcast/CRI formats
// ===================================================================

static const char *detect_extension(const std::vector<uint8_t> &data) {
    // ADX: big-endian 0x8000 at offset 0 + "(c)CRI" signature
    if (data.size() >= 24 &&
        data[0] == 0x80 && data[1] == 0x00) {
        uint16_t copr_off = (static_cast<uint16_t>(data[2]) << 8) | data[3];
        if (copr_off >= 2 && data.size() >= static_cast<size_t>(copr_off + 4)) {
            if (std::memcmp(&data[copr_off - 2], "(c)CRI", 6) == 0)
                return ".adx";
        }
    }
    // AHX: "AHX(" at offset 0
    if (data.size() >= 4 && std::memcmp(data.data(), "AHX(", 4) == 0)
        return ".ahx";
    // SFD (Sofdec): MPEG-PS start code 00 00 01 BA
    if (data.size() >= 4 &&
        data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x01 && data[3] == 0xBA)
        return ".sfd";
    // PVR: optional GBIX header, then "PVRT" chunk (Dreamcast little-endian)
    if (data.size() >= 16) {
        size_t pvrt_off = 0;
        if (std::memcmp(data.data(), "GBIX", 4) == 0) {
            uint32_t gbix_sz = data[4] | (static_cast<uint32_t>(data[5]) << 8) |
                               (static_cast<uint32_t>(data[6]) << 16) |
                               (static_cast<uint32_t>(data[7]) << 24);
            if (data.size() >= 8 + gbix_sz + 16)
                pvrt_off = 8 + gbix_sz;
            else
                return ".bin";
        }
        if (std::memcmp(&data[pvrt_off], "PVRT", 4) == 0)
            return ".pvr";
    }
    return ".bin";
}

// ===================================================================
// Extract all entries
// ===================================================================

static bool extract_all(const AFSArchive &afs, const std::string &out_dir,
                        bool numbered, bool detect) {
    std::ifstream f(afs.file_path, std::ios::binary);
    if (!f) return false;

    fs::create_directories(out_dir);

    std::vector<uint8_t> peek_buf;
    std::vector<std::string> out_names;
    out_names.reserve(afs.entry_count);

    for (uint32_t i = 0; i < afs.entry_count; ++i) {
        const auto &e = afs.entries[i];
        std::string name;

        if (e.is_null) {
            name = "_NULL_";
        } else if (numbered && !detect) {
            // -n alone: ALL files → bare NNNNNNNN
            char idx[16];
            snprintf(idx, sizeof(idx), "%08u", i);
            name = idx;

        } else if (numbered && detect) {
            // -n -d: ALL files → NNNNNNNN + extension
            char idx[16];
            snprintf(idx, sizeof(idx), "%08u", i);

            if (afs.has_attributes && !e.name.empty()) {
                auto dot = e.name.rfind('.');
                if (dot != std::string::npos && dot + 1 < e.name.size()) {
                    name = std::string(idx) + e.name.substr(dot);
                    goto dedup;
                }
            }
            f.seekg(e.offset);
            peek_buf.resize(std::min(e.size, 256u));
            f.read(reinterpret_cast<char *>(peek_buf.data()), peek_buf.size());
            name = std::string(idx) + detect_extension(peek_buf);

        } else if (afs.has_attributes && !e.name.empty()) {
            // Default + -d: use TOC name
            name = sanitize_name(e.name);
        } else if (detect) {
            // -d, no TOC name: NNNNNNNN + magic extension
            char idx[16];
            snprintf(idx, sizeof(idx), "%08u", i);
            f.seekg(e.offset);
            peek_buf.resize(std::min(e.size, 256u));
            f.read(reinterpret_cast<char *>(peek_buf.data()), peek_buf.size());
            name = std::string(idx) + detect_extension(peek_buf);
        } else {
            // Default, no TOC: bare NNNNNNNN
            char idx[16];
            snprintf(idx, sizeof(idx), "%08u", i);
            name = idx;
        }

    dedup:
        uint32_t dup = 0;
        for (const auto &prev : out_names) {
            if (prev == name) ++dup;
        }
        if (dup > 0) {
            auto dot = name.rfind('.');
            if (dot != std::string::npos && dot > 0) {
                name = name.substr(0, dot) + " (" + std::to_string(dup) + ")" + name.substr(dot);
            } else {
                name += " (" + std::to_string(dup) + ")";
            }
        }
        out_names.push_back(name);
    }

    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < afs.entry_count; ++i) {
        const auto &e = afs.entries[i];
        if (e.is_null) {
            std::cout << "[" << (i + 1) << "/" << afs.entry_count
                      << "] Null entry, skipping.\n";
            continue;
        }

        std::cout << "[" << (i + 1) << "/" << afs.entry_count
                  << "] Extracting: " << out_names[i]
                  << " (" << e.size << " bytes)\n";

        f.seekg(e.offset);
        buf.resize(e.size);
        f.read(reinterpret_cast<char *>(buf.data()), e.size);

        fs::path out_path = fs::path(out_dir) / out_names[i];
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            std::cerr << "Error: cannot write to " << out_path << "\n";
            return false;
        }
        out.write(reinterpret_cast<const char *>(buf.data()), e.size);
    }

    std::cout << "Extracted " << afs.entry_count << " entries to " << out_dir << "\n";
    return true;
}

// ===================================================================
// Save metadata JSON
// ===================================================================

static void save_metadata(const AFSArchive &afs, const std::string &meta_path) {
    std::ofstream m(meta_path);
    if (!m) return;

    m << "{\n";
    m << "  \"MetadataVersion\": 3,\n";
    m << "  \"HeaderMagicType\": \"" << afs.magic_label << "\",\n";
    m << "  \"AttributesInfoType\": \"" << afs.attr_info_label << "\",\n";
    m << "  \"AllAttributesContainEntrySize\": false,\n";
    m << "  \"EntryBlockAlignment\": " << afs.entry_block_alignment << ",\n";
    m << "  \"Entries\": [\n";
    for (uint32_t i = 0; i < afs.entry_count; ++i) {
        const auto &e = afs.entries[i];
        if (e.is_null) {
            m << "    { \"IsNull\": true, \"Name\": \"\", \"FileName\": \"\", \"CustomData\": 0 }";
        } else {
            std::string san = sanitize_name(e.name);
            m << "    { \"IsNull\": false, "
              << "\"Name\": \"" << e.name << "\", "
              << "\"FileName\": \"" << san << "\", "
              << "\"CustomData\": " << e.custom_data << " }";
        }
        if (i + 1 < afs.entry_count) m << ",";
        m << "\n";
    }
    m << "  ]\n";
    m << "}\n";
    std::cout << "Metadata saved to " << meta_path << "\n";
}

// ===================================================================
// Create AFS archive
// ===================================================================

static bool create_afs(const std::string &input_dir, const std::string &output_path) {
    std::string meta_path = strip_trailing_sep(input_dir) + ".json";
    std::vector<SourceEntry> source;
    std::string magic_label = "AFS_00";
    std::string attr_mode   = "InfoAtBeginning";
    uint32_t entry_block_align = MIN_ALIGNMENT;

    // ---- load metadata if present ----
    if (fs::exists(meta_path)) {
        std::ifstream mf(meta_path);
        if (!mf) {
            std::cerr << "Warning: cannot read metadata \"" << meta_path << "\", using defaults.\n";
        } else {
            JValue root = parse_json(mf);
            if (root.is_object()) {
                std::string mt = root.str("HeaderMagicType");
                if (mt == "AFS_20") magic_label = "AFS_20";
                else if (mt == "AFS_00" || mt.empty()) magic_label = "AFS_00";

                std::string at = root.str("AttributesInfoType");
                if (at == "InfoAtEnd")      attr_mode = "InfoAtEnd";
                else if (at == "NoAttributes") attr_mode = "NoAttributes";
                else                        attr_mode = "InfoAtBeginning";

                int al = root.num("EntryBlockAlignment");
                if (al > 0) entry_block_align = std::max(static_cast<uint32_t>(al), MIN_ALIGNMENT);

                const JValue &entries = root["Entries"];
                if (entries.is_array() && entries.size() > 0) {
                    for (size_t i = 0; i < entries.size(); ++i) {
                        const JValue &je = entries[i];
                        SourceEntry se;
                        se.is_null = je.flag("IsNull");
                        se.entry_name = je.str("Name");
                        se.file_name  = je.str("FileName");
                        se.custom_data = static_cast<uint32_t>(je.num("CustomData"));
                        source.push_back(std::move(se));
                    }
                }
            }
            std::cout << "Loaded metadata: " << source.size() << " entries, "
                      << magic_label << ", " << attr_mode
                      << ", alignment=0x" << std::hex << entry_block_align << std::dec << "\n";
        }
    }

    // ---- if no metadata entries, scan directory ----
    if (source.empty()) {
        std::cout << "No metadata found, scanning directory for files...\n";
        std::vector<fs::path> files;
        for (const auto &de : fs::directory_iterator(input_dir)) {
            if (de.is_regular_file()) files.push_back(de.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto &fp : files) {
            SourceEntry se;
            se.file_name = fp.filename().string();
            se.entry_name = se.file_name;
            if (se.entry_name.size() > MAX_NAME_LEN) {
                std::string ext = fp.extension().string();
                se.entry_name = se.entry_name.substr(0, MAX_NAME_LEN - ext.size()) + ext;
                std::cout << "Warning: \"" << se.file_name << "\" truncated to \"" << se.entry_name << "\"\n";
            }
            se.custom_data = static_cast<uint32_t>(fs::file_size(fp));
            source.push_back(std::move(se));
        }
    }

    if (source.empty()) {
        std::cerr << "Error: no entries to pack.\n";
        return false;
    }

    // ---- read all file data into memory ----
    std::vector<std::vector<uint8_t>> file_data(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        const auto &se = source[i];
        if (se.is_null) continue;

        fs::path file_path = fs::path(input_dir) / se.file_name;
        uint64_t fsize = fs::file_size(file_path);
        file_data[i].resize(fsize);
        std::ifstream fin(file_path, std::ios::binary);
        if (!fin) {
            std::cerr << "Error: cannot read \"" << file_path << "\"\n";
            return false;
        }
        fin.read(reinterpret_cast<char *>(file_data[i].data()), fsize);
    }

    // ---- calculate offsets ----
    uint32_t entry_count = static_cast<uint32_t>(source.size());
    uint32_t first_entry_offset = pad(HEADER_SIZE + entry_count * ENTRY_INFO_SZ + ATTR_INFO_SZ, entry_block_align);

    std::vector<uint32_t> offsets(entry_count);
    std::vector<uint32_t> sizes(entry_count);

    uint32_t next = first_entry_offset;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (source[i].is_null) {
            offsets[i] = 0;
            sizes[i] = 0;
        } else {
            offsets[i] = next;
            sizes[i] = static_cast<uint32_t>(file_data[i].size());
            next = pad(next + sizes[i], DATA_ALIGNMENT);
        }
    }

    uint32_t attr_offset = next;
    bool has_attr = (attr_mode != "NoAttributes");
    uint32_t attr_size = has_attr ? entry_count * ATTR_ELEM_SZ : 0;
    uint32_t eof = has_attr ? pad(attr_offset + attr_size, DATA_ALIGNMENT)
                            : pad(attr_offset, DATA_ALIGNMENT);

    // ---- write ----
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot create \"" << output_path << "\"\n";
        return false;
    }

    // header
    uint32_t magic = (magic_label == "AFS_20") ? AFS_MAGIC_20 : AFS_MAGIC_00;
    write_u32_le(out, magic);
    write_u32_le(out, entry_count);

    // entry info block
    for (uint32_t i = 0; i < entry_count; ++i) {
        write_u32_le(out, offsets[i]);
        write_u32_le(out, sizes[i]);
    }

    // attributes info (at beginning or end)
    uint32_t pos_after_entries = HEADER_SIZE + entry_count * ENTRY_INFO_SZ;
    uint32_t gap_to_data = first_entry_offset - (pos_after_entries + ATTR_INFO_SZ);

    if (has_attr && attr_mode == "InfoAtBeginning") {
        write_u32_le(out, attr_offset);
        write_u32_le(out, attr_size);
        fill_zeroes(out, gap_to_data);
    } else if (has_attr && attr_mode == "InfoAtEnd") {
        fill_zeroes(out, gap_to_data);
        write_u32_le(out, attr_offset);
        write_u32_le(out, attr_size);
    } else {
        write_u32_le(out, 0);
        write_u32_le(out, 0);
        fill_zeroes(out, gap_to_data);
    }

    // entry data
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (source[i].is_null) {
            std::cout << "[" << (i + 1) << "/" << entry_count << "] Null entry\n";
            continue;
        }
        std::cout << "[" << (i + 1) << "/" << entry_count
                  << "] Writing: " << source[i].entry_name
                  << " (" << sizes[i] << " bytes)\n";

        out.seekp(offsets[i]);
        out.write(reinterpret_cast<const char *>(file_data[i].data()), sizes[i]);
    }

    // attribute data
    if (has_attr) {
        out.seekp(attr_offset);
        for (uint32_t i = 0; i < entry_count; ++i) {
            if (source[i].is_null) {
                fill_zeroes(out, ATTR_ELEM_SZ);
            } else {
                std::string name = source[i].entry_name;
                if (name.size() > MAX_NAME_LEN) name = name.substr(0, MAX_NAME_LEN);
                name.resize(MAX_NAME_LEN, '\0');
                out.write(name.data(), MAX_NAME_LEN);

                struct tm tmbuf{};
                std::time_t tt = std::time(nullptr);
#if defined(_WIN32)
                struct tm *tm = gmtime(&tt);
                if (tm) tmbuf = *tm;
#else
                gmtime_r(&tt, &tmbuf);
#endif
                write_u16_le(out, static_cast<uint16_t>(tmbuf.tm_year + 1900));
                write_u16_le(out, static_cast<uint16_t>(tmbuf.tm_mon + 1));
                write_u16_le(out, static_cast<uint16_t>(tmbuf.tm_mday));
                write_u16_le(out, static_cast<uint16_t>(tmbuf.tm_hour));
                write_u16_le(out, static_cast<uint16_t>(tmbuf.tm_min));
                write_u16_le(out, static_cast<uint16_t>(tmbuf.tm_sec));
                write_u32_le(out, source[i].custom_data);
            }
        }
    }

    // final padding + truncate
    out.seekp(0, std::ios::end);
    uint32_t current_end = static_cast<uint32_t>(out.tellp());
    if (current_end < eof) fill_zeroes(out, eof - current_end);

    // ensure exact size (in case stream was larger from previous content)
    // std::ofstream doesn't have setLength; we rely on the file being created fresh

    out.close();
    std::cout << "Created " << output_path << " (" << eof << " bytes, "
              << entry_count << " entries)\n";
    return true;
}

// ===================================================================
// Show info
// ===================================================================

static void show_info(const AFSArchive &afs) {
    std::cout << "\n";
    std::cout << " File name             : " << fs::path(afs.file_path).filename().string() << "\n";
    std::cout << " Header magic          : " << afs.magic_label << "\n";
    std::cout << " Attributes info type  : " << afs.attr_info_label << "\n";
    std::cout << " Entry Block Alignment : 0x" << std::hex << afs.entry_block_alignment << std::dec << "\n";
    std::cout << " Number of entries     : " << afs.entry_count << "\n\n";

    std::cout << " Index    | Name                             | Size       | Custom Data | Last Write Time\n";
    std::cout << " -----------------------------------------------------------------------------------------------------\n";

    for (uint32_t i = 0; i < afs.entry_count; ++i) {
        const auto &e = afs.entries[i];
        if (e.is_null) {
            std::cout << " " << std::right << std::setw(8) << std::setfill('0') << i
                      << std::setfill(' ') << " | "
                      << std::left << std::setw(32) << "(null)" << " | "
                      << std::setw(10) << "N/A" << " | "
                      << std::setw(11) << "N/A" << " | "
                      << "N/A\n";
        } else {
            std::ostringstream ss;
            ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << e.size;
            std::string size_str = "0x" + ss.str();

            std::string cd_str = afs.has_attributes
                ? ("0x" + ([] (uint32_t v) {
                    std::ostringstream s2;
                    s2 << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
                    return s2.str();
                }(e.custom_data)))
                : "N/A";

            std::string ts = "N/A";
            if (afs.has_attributes) {
                std::ostringstream s3;
                s3 << e.year << "/"
                   << std::setw(2) << std::setfill('0') << e.month << "/"
                   << std::setw(2) << std::setfill('0') << e.day << " "
                   << std::setw(2) << std::setfill('0') << e.hour << ":"
                   << std::setw(2) << std::setfill('0') << e.min << ":"
                   << std::setw(2) << std::setfill('0') << e.sec;
                ts = s3.str();
            }

            std::string name_disp = afs.has_attributes ? e.name : "N/A";
            if (name_disp.length() > 32) name_disp = name_disp.substr(0, 32);

            std::cout << " " << std::right << std::setw(8) << std::setfill('0') << i
                      << std::setfill(' ') << " | "
                      << std::left << std::setw(32) << name_disp << " | "
                      << std::setw(10) << size_str << " | "
                      << std::setw(11) << cd_str << " | "
                      << ts << "\n";
        }
    }
    std::cout << std::right;
}

// ===================================================================
// Usage
// ===================================================================

static void usage(const char *prog) {
    std::cout << "\nAFS CLI - AFS archive packer / unpacker\n\n";
    std::cout << "Usage:\n\n";
    std::cout << "  " << prog << " -e <input.afs> <output_dir>   Extract AFS archive\n";
    std::cout << "  " << prog << " -e -d <input.afs> <output_dir> Extract, detect types for nameless\n";
    std::cout << "  " << prog << " -e -n <input.afs> <output_dir> Extract, number all filenames\n";
    std::cout << "  " << prog << " -e -n -d <input.afs> <output>  Extract, numbered + type detection\n";
    std::cout << "  " << prog << " -c <input_dir> <output.afs>   Create AFS archive\n";
    std::cout << "  " << prog << " -i <input.afs>                 Show AFS information\n\n";
}

// ===================================================================
int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 0;
    }

    std::string mode(argv[1]);

    if (mode == "-e") {
        bool numbered = false, detect = false;
        std::string input, output;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "-n") == 0)      { numbered = true; }
            else if (std::strcmp(argv[i], "-d") == 0) { detect = true; }
            else if (input.empty())  { input = argv[i]; }
            else if (output.empty()) { output = argv[i]; }
        }
        if (input.empty() || output.empty()) { usage(argv[0]); return 1; }

        AFSArchive afs;
        if (!load_afs(input, afs)) return 1;
        if (!extract_all(afs, output, numbered, detect)) return 1;

        save_metadata(afs, strip_trailing_sep(output) + ".json");

    } else if (mode == "-c") {
        if (argc != 4) { usage(argv[0]); return 1; }
        std::string input(argv[2]), output(argv[3]);

        if (!create_afs(input, output)) return 1;

    } else if (mode == "-i") {
        if (argc != 3) { usage(argv[0]); return 1; }
        std::string input(argv[2]);

        AFSArchive afs;
        if (!load_afs(input, afs)) return 1;
        show_info(afs);

    } else {
        usage(argv[0]);
        return 1;
    }

    return 0;
}
