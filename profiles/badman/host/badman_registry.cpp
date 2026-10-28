#include "badman_registry.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace badman {
namespace {
constexpr std::uint32_t invalid = 0x80010016u, bad_handle = 0x80010009u;
constexpr std::uint32_t missing = 0x80010002u, read_only = 0x8001001Eu;
constexpr std::uint32_t registry_missing = 0x80082715u, category_missing = 0x80082718u;
constexpr std::uint32_t permission = 0x8008273Bu, no_memory = 0x8001000Cu;

std::uint32_t le32(const std::uint8_t *p) {
    return p[0] | (std::uint32_t(p[1]) << 8u) | (std::uint32_t(p[2]) << 16u) | (std::uint32_t(p[3]) << 24u);
}
bool guest_string(const psprecomp::GuestMemory &memory, std::uint32_t address,
                  std::size_t maximum, std::string &out) {
    out.clear();
    for (std::size_t i = 0; i < maximum; ++i) {
        if (i > std::numeric_limits<std::uint32_t>::max() - address || !memory.contains(address + static_cast<std::uint32_t>(i))) return false;
        const auto c = memory.load8(address + static_cast<std::uint32_t>(i));
        if (!c) return true;
        out.push_back(static_cast<char>(c));
    }
    return false;
}
bool word_output(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return (address & 3u) == 0u && memory.contains(address, 4u);
}
std::string header_string(const std::array<std::uint8_t, 0x188> &header, std::size_t offset) {
    std::string result;
    for (std::size_t i = offset; i < offset + 64u; ++i) {
        if (!header[i]) return result;
        if (header[i] < 0x20u || header[i] > 0x7Eu) throw psprecomp::Error("PGF metadata contains a non-ASCII font name/style");
        result.push_back(static_cast<char>(header[i]));
    }
    throw psprecomp::Error("PGF metadata name/style is not terminated");
}
}

void Registry::configure_font_directory(const std::filesystem::path &directory) {
    if (!registries_.empty() || !categories_.empty() || !keys_.empty()) throw psprecomp::Error("Cannot replace a registry with open handles");
    const auto root = std::filesystem::weakly_canonical(directory);
    if (!std::filesystem::is_directory(root)) throw psprecomp::Error("Font registry needs an existing font directory");
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".pgf") continue;
        const auto path = std::filesystem::weakly_canonical(entry.path());
        if (path.parent_path() != root || !std::filesystem::is_regular_file(path)) throw psprecomp::Error("Font registry rejects an escaping/non-file PGF path");
        const auto name = entry.path().filename().string();
        if (name.empty() || name.size() >= 64u || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. ") != std::string::npos)
            throw psprecomp::Error("Font filename cannot be represented safely in the registry");
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() > 128u) throw psprecomp::Error("Font registry configured with too many fonts");
    std::map<std::string, CategoryValues> values;
    auto integer = [](std::uint32_t value) {
        Value result{2u, std::vector<std::uint8_t>(4u)};
        for (unsigned i = 0; i < 4u; ++i) result.bytes[i] = static_cast<std::uint8_t>(value >> (i * 8u));
        return result;
    };
    auto string = [](const std::string &text) {
        Value result{3u, {text.begin(), text.end()}}; result.bytes.push_back(0u); return result;
    };
    values["/DATA/FONT"]["path_name"] = string("flash0:/font/");
    values["/DATA/FONT"]["num_fonts"] = integer(static_cast<std::uint32_t>(files.size()));
    // Read the stored 26.6 dimensions/resolution and embedded name/style. The
    // other selection attributes below are explicit compatibility defaults,
    // not claims that the PGF header stores region/weight/expiry properties.
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::array<std::uint8_t, 0x188> header{};
        std::ifstream input(files[i], std::ios::binary);
        if (!input.read(reinterpret_cast<char *>(header.data()), header.size()) ||
            header[2] != 0x88u || header[3] != 1u || !std::equal(header.begin()+4, header.begin()+8, "PGF0") ||
            le32(header.data()+8) != 2u || le32(header.data()+12) != 6u)
            throw psprecomp::Error("Font registry requires a recognized revision-2/version-6 PGF header: " + files[i].string());
        const auto name = header_string(header, 0x35u), style = header_string(header, 0x75u);
        if (name.empty()) throw psprecomp::Error("PGF metadata has no font name");
        std::uint32_t family = 0u, style_code = 0u, language = 0u;
        if (name.find("NewRodin") != std::string::npos) family = 1u;
        else if (name.find("Matisse") != std::string::npos) family = 2u;
        if (style == "Regular") style_code = 1u;
        else if (style == "Italic") style_code = 2u;
        else if (style == "Bold") style_code = 5u;
        else if (style == "Bold Italic" || style == "Italic Bold") style_code = 6u;
        const auto filename = files[i].filename().string();
        if (filename.starts_with("jpn")) language = 1u;
        else if (filename.starts_with("ltn")) language = 2u;
        else if (filename.starts_with("kr")) language = 3u;
        auto &font = values["/DATA/FONT/PROPERTY/INFO" + std::to_string(i)];
        const std::array<const char *, 4> metrics{"h_size", "v_size", "h_resolution", "v_resolution"};
        for (std::size_t field = 0; field < metrics.size(); ++field) {
            const auto value = le32(header.data()+0x24u+field*4u);
            if (value == 0u || value > 0x100000u) throw psprecomp::Error("Invalid PGF 26.6 metric");
            font[metrics[field]] = integer(value);
        }
        font["weight"] = integer(0u); font["family_code"] = integer(family);
        font["style"] = integer(style_code); font["sub_style"] = integer(0u);
        font["language_code"] = integer(language); font["region_code"] = integer(0u);
        font["country_code"] = integer(0u); font["font_name"] = string(name);
        font["file_name"] = string(filename); font["extra_attributes"] = integer(0u);
        font["expire_date"] = integer(0u);
        if (i == 0u) {
            values["/SYSPROFILE/RESOLUTION"]["horizontal"] = font["h_resolution"];
            values["/SYSPROFILE/RESOLUTION"]["vertical"] = font["v_resolution"];
        }
    }
    values_ = std::move(values); font_count_ = files.size(); configured_ = true;
}

void Registry::close_category(std::uint32_t handle) {
    std::erase_if(keys_, [handle](const auto &entry) { return entry.second.category == handle; });
    categories_.erase(handle);
}

void Registry::invoke(std::uint32_t nid, psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    diagnostic_.clear();
    context.gpr[2] = call(nid, runtime.memory(), context);
}

std::uint32_t Registry::call(std::uint32_t nid, psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    auto fail = [this](std::uint32_t error, const std::string &reason) { diagnostic_ = reason; return error; };
    const auto a0=ctx.gpr[4], a1=ctx.gpr[5], a2=ctx.gpr[6], a3=ctx.gpr[7];
    if (nid == 0x92E41280u) { // sceRegOpenRegistry
        if ((a0 & 3u) != 0u || !memory.contains(a0,0x110u) || !word_output(memory,a2)) return fail(invalid,"Invalid registry parameter/output buffer");
        std::string name;
        if (!guest_string(memory,a0+4u,256u,name)) return fail(invalid,"Unterminated registry name");
        if (a1 != 2u) return fail(a1==1u?permission:invalid,"Only read-only registry mode 2 is supported");
        if (!configured_) return fail(registry_missing,"No font directory configured for registry");
        const auto length=memory.load32(a0+0x104u);
        if (memory.load32(a0)!=1u || (length!=name.size() && length!=name.size()+1u) ||
            memory.load32(a0+0x108u)>1u || memory.load32(a0+0x10cu)>1u) return fail(invalid,"Invalid system RegParam");
        if (!name.empty() && name!="/system") return fail(registry_missing,"Unknown registry: "+name);
        if (next_handle_ == std::numeric_limits<std::uint32_t>::max() || registries_.size()+categories_.size()+keys_.size()>=4096u) return fail(no_memory,"Registry handle limit");
        const auto handle=next_handle_++; registries_.insert(handle); memory.store32(a2,handle); return 0u;
    }
    if (nid == 0xFA8A5739u) { // sceRegCloseRegistry
        if (!registries_.contains(a0)) return fail(bad_handle,"Unknown/closed registry handle");
        for (auto it=categories_.begin();it!=categories_.end();) {
            const auto handle=it->first; const bool owned=it->second.registry==a0; ++it;
            if (owned) close_category(handle);
        }
        registries_.erase(a0); return 0u;
    }
    if (nid == 0x1D8A762Eu) { // sceRegOpenCategory
        if (!registries_.contains(a0)) return fail(bad_handle,"Invalid registry for category open");
        if (!word_output(memory,a3)) return fail(invalid,"Invalid category output handle");
        std::string path;
        if (!guest_string(memory,a1,256u,path)) return fail(invalid,"Invalid category path buffer");
        if (a2!=2u) return fail(a2==1u?permission:invalid,"Only read-only category mode 2 is supported");
        if (!values_.contains(path)) return fail(category_missing,"Unknown registry category: "+path);
        if (next_handle_ == std::numeric_limits<std::uint32_t>::max() || registries_.size()+categories_.size()+keys_.size()>=4096u) return fail(no_memory,"Registry handle limit");
        const auto handle=next_handle_++; categories_.emplace(handle,Category{a0,path}); memory.store32(a3,handle); return 0u;
    }
    if (nid == 0x0CAE832Bu) { // sceRegCloseCategory
        if (!categories_.contains(a0)) return fail(bad_handle,"Unknown/closed category handle");
        close_category(a0); return 0u;
    }
    if (nid == 0xD4475AA8u) { // sceRegGetKeyInfo; fifth argument is Allegrex t0
        const auto category=categories_.find(a0);
        if (category==categories_.end()) return fail(bad_handle,"Invalid category for key lookup");
        const auto size_address=ctx.gpr[8];
        if (!word_output(memory,a2) || !word_output(memory,a3) || !word_output(memory,size_address) ||
            psprecomp::GuestMemory::canonical(a2)==psprecomp::GuestMemory::canonical(a3) ||
            psprecomp::GuestMemory::canonical(a2)==psprecomp::GuestMemory::canonical(size_address) ||
            psprecomp::GuestMemory::canonical(a3)==psprecomp::GuestMemory::canonical(size_address)) return fail(invalid,"Invalid/aliased key info outputs");
        std::string name;
        if (!guest_string(memory,a1,28u,name)) return fail(invalid,"Invalid registry key name buffer");
        const auto &values=values_.at(category->second.path); const auto value=values.find(name);
        if (value==values.end()) return fail(missing,"Unknown registry key: "+category->second.path+"/"+name);
        if (next_handle_ == std::numeric_limits<std::uint32_t>::max() || registries_.size()+categories_.size()+keys_.size()>=4096u) return fail(no_memory,"Registry handle limit");
        const auto handle=next_handle_++; keys_.emplace(handle,Key{a0,name});
        memory.store32(a2,handle); memory.store32(a3,value->second.type);
        memory.store32(size_address,static_cast<std::uint32_t>(value->second.bytes.size())); return 0u;
    }
    if (nid == 0x28A8E98Au) { // sceRegGetKeyValue
        const auto category=categories_.find(a0); const auto key=keys_.find(a1);
        if (category==categories_.end() || key==keys_.end() || key->second.category!=a0) return fail(bad_handle,"Key does not belong to this open category");
        const auto &value=values_.at(category->second.path).at(key->second.name);
        if (a3<value.bytes.size() || !memory.contains(a2,a3)) return fail(invalid,"Invalid/undersized registry value buffer");
        for (std::size_t i=0;i<value.bytes.size();++i) memory.store8(a2+static_cast<std::uint32_t>(i),value.bytes[i]);
        return 0u;
    }
    return fail(read_only,"Registry operation is outside the read-only font subset: "+psprecomp::hex32(nid));
}
}
