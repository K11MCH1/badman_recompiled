#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace badman {
// Read-only, explicitly configured font registry. No firmware registry is bundled.
class Registry {
public:
    Registry() = default;
    explicit Registry(const std::filesystem::path &font_directory) { configure_font_directory(font_directory); }
    void configure_font_directory(const std::filesystem::path &font_directory);
    void invoke(std::uint32_t nid, psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context);
    std::size_t font_count() const { return font_count_; }
    const std::string &last_diagnostic() const { return diagnostic_; }
private:
    struct Value { std::uint32_t type{}; std::vector<std::uint8_t> bytes; };
    using CategoryValues = std::map<std::string, Value>;
    struct Category { std::uint32_t registry; std::string path; };
    struct Key { std::uint32_t category; std::string name; };
    std::map<std::string, CategoryValues> values_;
    std::set<std::uint32_t> registries_;
    std::map<std::uint32_t, Category> categories_;
    std::map<std::uint32_t, Key> keys_;
    std::uint32_t next_handle_{0x5000};
    std::size_t font_count_{};
    bool configured_{};
    std::string diagnostic_;
    std::uint32_t call(std::uint32_t nid, psprecomp::GuestMemory &, psprecomp::AllegrexContext &);
    void close_category(std::uint32_t handle);
};
}
