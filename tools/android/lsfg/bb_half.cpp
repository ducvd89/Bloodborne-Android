// Bloodframe: see bb_half.hpp.
#include "bb_half.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "extract/extract.hpp"

namespace BbHalf {

namespace {

// The half-precision module of a shader is the shader's DXBC resource id plus this.
constexpr uint32_t ResourceOffset = 49;

constexpr uint32_t SpirvMagic = 0x07230203u;
constexpr uint32_t SpirvMaxVersion = 0x00010600u;
constexpr size_t HeaderWords = 5;

constexpr uint32_t OpExtension = 10;
constexpr uint32_t OpCapability = 17;
constexpr uint32_t OpDecorate = 71;
constexpr uint32_t DecorationBinding = 33;
constexpr uint32_t DecorationDescriptorSet = 34;

// The capabilities the half-precision modules may use. Of these the framegen's device already has to
// have all but Float16 (its capability check refuses a GPU without the storage image features), and
// Float16 is the one feature enabled on request (LSFG::setShaderFloat16Requested). Anything else, such
// as 16-bit storage or Int16, is a module this build was not checked against: full precision then.
const std::map<uint32_t, const char*> AllowedCapabilities{
    {1, "Shader"},
    {9, "Float16"},
    {49, "StorageImageExtendedFormats"},
    {50, "ImageQuery"},
    {56, "StorageImageWriteWithoutFormat"},
};

std::unordered_map<std::string, std::vector<uint8_t>> g_modules;
std::set<uint32_t> g_capabilities;

// The dll numbers the bindings by resource class (constants 0, samplers 16, sampled images 32,
// storage images 48): the descriptor layouts of the framegen are consecutive from 0 in the same
// order, so each binding becomes its rank among the module's bindings (what the DXBC translation
// does too).
std::string Renumber(std::vector<uint32_t>& words, std::set<uint32_t>& capabilities) {
    std::vector<std::pair<uint32_t, size_t>> bindings; // value, index of the value word
    size_t at = HeaderWords;
    while (at < words.size()) {
        const uint32_t count = words[at] >> 16;
        const uint32_t opcode = words[at] & 0xffffu;
        if (count == 0 || at + count > words.size()) {
            return "a module has a malformed instruction";
        }
        if (opcode == OpExtension) {
            return "a module uses a SPIR-V extension";
        }
        if (opcode == OpCapability && count >= 2) {
            const uint32_t capability = words[at + 1];
            if (AllowedCapabilities.count(capability) == 0) {
                return "a module needs SPIR-V capability " + std::to_string(capability);
            }
            capabilities.insert(capability);
        } else if (opcode == OpDecorate && count >= 4) {
            if (words[at + 2] == DecorationBinding) {
                bindings.emplace_back(words[at + 3], at + 3);
            } else if (words[at + 2] == DecorationDescriptorSet && words[at + 3] != 0) {
                return "a module uses a descriptor set other than 0";
            }
        }
        at += count;
    }
    std::sort(bindings.begin(), bindings.end());
    for (size_t i = 1; i < bindings.size(); ++i) {
        if (bindings[i].first == bindings[i - 1].first) {
            return "a module binds two resources to one binding";
        }
    }
    for (size_t rank = 0; rank < bindings.size(); ++rank) {
        words[bindings[rank].second] = static_cast<uint32_t>(rank);
    }
    return {};
}

} // namespace

std::string Prepare() {
    g_modules.clear();
    g_capabilities.clear();
    for (const std::string& name : Extract::getShaderNames()) {
        const uint32_t dxbc_id = Extract::getShaderId(name);
        const uint32_t id = dxbc_id + ResourceOffset;
        const std::vector<uint8_t> bytes = Extract::getResource(id);
        const std::string what = name + " (resource " + std::to_string(id) + ")";
        if (bytes.size() < HeaderWords * 4 || bytes.size() % 4 != 0) {
            g_modules.clear();
            return "this Lossless.dll has no half-precision shader for " + what;
        }
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        if (words[0] != SpirvMagic || words[1] > SpirvMaxVersion) {
            g_modules.clear();
            return "the half-precision shader " + what + " is not SPIR-V this build knows";
        }
        std::set<uint32_t> capabilities;
        const std::string why = Renumber(words, capabilities);
        if (!why.empty()) {
            g_modules.clear();
            return why + ": " + what;
        }
        g_capabilities.insert(capabilities.begin(), capabilities.end());
        std::vector<uint8_t> module(bytes.size());
        std::memcpy(module.data(), words.data(), module.size());
        g_modules.emplace(name, std::move(module));
    }
    return {};
}

std::vector<uint8_t> Get(const std::string& name) {
    const auto hit = g_modules.find(name);
    if (hit == g_modules.end()) {
        throw std::runtime_error("No half-precision shader: " + name);
    }
    return hit->second;
}

std::string Capabilities() {
    std::string text;
    for (const uint32_t capability : g_capabilities) {
        text += (text.empty() ? "" : " ") + std::string(AllowedCapabilities.at(capability));
    }
    return text;
}

} // namespace BbHalf
