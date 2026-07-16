#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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

// ---------------------------------------------------------------------------
// Little-endian read helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Entry record from the file
// ---------------------------------------------------------------------------
struct EntryInfo {
    uint32_t offset = 0;
    uint32_t size   = 0;
    std::string name;
    uint16_t year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    uint32_t custom_data = 0;
    bool is_null = false;
};

// ---------------------------------------------------------------------------
// AFS archive (in-memory descriptors)
// ---------------------------------------------------------------------------
struct AFSArchive {
    std::string magic_label;       // "AFS_00" or "AFS_20"
    uint32_t entry_count = 0;
    uint32_t entry_block_alignment = MIN_ALIGNMENT;
    std::string attr_info_label;   // "NoAttributes", "InfoAtBeginning", "InfoAtEnd"
    bool has_attributes = false;
    std::vector<EntryInfo> entries;
    std::string file_path;
};

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
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
        // strip characters that are problematic on any platform
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            continue;
        out.push_back(c);
    }
    if (out.empty()) return "_NO_NAME";
    return out;
}

// ---------------------------------------------------------------------------
// Load AFS archive and parse headers (does not copy entry data into memory)
// ---------------------------------------------------------------------------
static bool load_afs(const std::string &path, AFSArchive &afs) {
    afs.file_path = path;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Error: cannot open file \"" << path << "\"\n";
        return false;
    }

    // --- header ---
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

    // --- entry info block ---
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

    // --- calculate entry block alignment ---
    uint32_t end_of_info = HEADER_SIZE + afs.entry_count * ENTRY_INFO_SZ + ATTR_INFO_SZ;
    uint32_t alignment = MIN_ALIGNMENT;
    while (end_of_info + alignment < first_offset) alignment <<= 1;
    afs.entry_block_alignment = alignment;

    // --- attribute info (try at-beginning location first) ---
    afs.has_attributes = false;
    afs.attr_info_label = "NoAttributes";

    f.seekg(end_of_info - ATTR_INFO_SZ);
    uint32_t attr_offset = read_le<uint32_t>(f);
    uint32_t attr_size   = read_le<uint32_t>(f);
    uint64_t file_size   = fs::file_size(path);

    auto is_valid_attr = [&]() -> bool {
        if (attr_offset == 0 || attr_size == 0) return false;
        if (attr_size > file_size - last_end) return false;
        if (attr_size < afs.entry_count * ATTR_ELEM_SZ) return false;
        if (attr_offset < last_end) return false;
        if (attr_offset > file_size - attr_size) return false;
        return true;
    };

    if (is_valid_attr()) {
        afs.has_attributes = true;
        afs.attr_info_label = "InfoAtBeginning";
    } else {
        // try at-end location
        f.seekg(first_offset - ATTR_INFO_SZ);
        attr_offset = read_le<uint32_t>(f);
        attr_size   = read_le<uint32_t>(f);
        if (is_valid_attr()) {
            afs.has_attributes = true;
            afs.attr_info_label = "InfoAtEnd";
        }
    }

    // --- read attribute data ---
    if (afs.has_attributes) {
        f.seekg(attr_offset);
        std::vector<uint8_t> attr_buf(attr_size);
        f.read(reinterpret_cast<char *>(attr_buf.data()), attr_size);

        const uint8_t *p = attr_buf.data();
        for (uint32_t i = 0; i < afs.entry_count; ++i) {
            auto &e = afs.entries[i];
            if (!e.is_null) {
                e.name  = null_terminated(p, MAX_NAME_LEN);
                e.year  = read_u16_le(p + 32);
                e.month = read_u16_le(p + 34);
                e.day   = read_u16_le(p + 36);
                e.hour  = read_u16_le(p + 38);
                e.min   = read_u16_le(p + 40);
                e.sec   = read_u16_le(p + 42);
                e.custom_data = read_u32_le(p + 44);
            }
            p += ATTR_ELEM_SZ;
        }
    } else {
        // no attributes: generate numbered names
        for (uint32_t i = 0; i < afs.entry_count; ++i) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%08u", i);
            afs.entries[i].name = buf;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Extract all entries
// ---------------------------------------------------------------------------
static bool extract_all(const AFSArchive &afs, const std::string &out_dir) {
    std::ifstream f(afs.file_path, std::ios::binary);
    if (!f) return false;

    fs::create_directories(out_dir);

    // detect and resolve duplicate sanitized names
    std::vector<std::string> out_names;
    out_names.reserve(afs.entry_count);
    for (const auto &e : afs.entries) {
        std::string san = e.is_null ? "_NULL_" : sanitize_name(e.name);
        uint32_t dup = 0;
        for (const auto &prev : out_names) {
            if (prev == san) ++dup;
        }
        if (dup > 0) {
            auto dot = san.rfind('.');
            if (dot != std::string::npos && dot > 0) {
                san = san.substr(0, dot) + " (" + std::to_string(dup) + ")" + san.substr(dot);
            } else {
                san += " (" + std::to_string(dup) + ")";
            }
        }
        out_names.push_back(san);
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
        out.close();

        // restore timestamp if available
        if (afs.has_attributes && (e.year != 0 || e.month != 0)) {
            // best-effort: no standard C++ way to set file times portably
            // on Linux/macOS we could use utimes, but skip for simplicity
        }
    }

    std::cout << "Extracted " << afs.entry_count << " entries to " << out_dir << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Save metadata JSON (for re-packing compatibility)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Show info
// ---------------------------------------------------------------------------
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
            std::string size_str = "0x";
            {
                std::ostringstream ss;
                ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << e.size;
                size_str += ss.str();
            }
            std::string cd_str = afs.has_attributes
                ? ("0x" + ([] (uint32_t v) {
                    std::ostringstream ss;
                    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
                    return ss.str();
                }(e.custom_data)))
                : "N/A";
            std::string ts = afs.has_attributes
                ? ([] (uint16_t y, uint16_t mo, uint16_t d, uint16_t h, uint16_t mi, uint16_t s) {
                    std::ostringstream ss;
                    ss << y << "/" << (mo < 10 ? "0" : "") << mo << "/"
                       << (d < 10 ? "0" : "") << d << " "
                       << (h < 10 ? "0" : "") << h << ":"
                       << (mi < 10 ? "0" : "") << mi << ":"
                       << (s < 10 ? "0" : "") << s;
                    return ss.str();
                }(e.year, e.month, e.day, e.hour, e.min, e.sec))
                : "N/A";

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

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void usage(const char *prog) {
    std::cout << "\nAFS CLI - AFS archive unpacker (C++ port)\n\n";
    std::cout << "Usage:\n\n";
    std::cout << "  " << prog << " -e <input.afs> <output_dir>   Extract AFS archive\n";
    std::cout << "  " << prog << " -i <input.afs>                  Show AFS information\n\n";
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 0;
    }

    std::string mode(argv[1]);

    if (mode == "-e") {
        if (argc != 4) { usage(argv[0]); return 1; }
        std::string input(argv[2]), output(argv[3]);

        AFSArchive afs;
        if (!load_afs(input, afs)) return 1;
        if (!extract_all(afs, output)) return 1;

        save_metadata(afs, output + ".json");

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
