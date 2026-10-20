// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hha::abi {
namespace detail {
// Probe only library-owned local objects. A different layout must fail without
// following an unchecked map pointer. Ignore Android's top-byte pointer tags
// for mapping comparisons while preserving the original pointer for reads.
class Readable {
    struct Range { uintptr_t begin, end; };
    std::vector<Range> ranges_;
public:
    Readable() {
        FILE* file = std::fopen("/proc/self/maps", "re");
        if (!file) return;
        char line[1024], access[5];
        unsigned long long begin, end;
        while (std::fgets(line, sizeof(line), file)) {
            if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, access) == 3 && access[0] == 'r')
                ranges_.push_back({static_cast<uintptr_t>(begin), static_cast<uintptr_t>(end)});
        }
        std::fclose(file);
    }
    bool Copy(uintptr_t pointer, void* output, size_t length) const {
        const uintptr_t address = pointer & UINT64_C(0x00ffffffffffffff);
        for (const auto& range : ranges_) {
            if (address >= range.begin && address < range.end && length <= range.end-address) {
                std::memcpy(output, reinterpret_cast<const void*>(pointer), length);
                return true;
            }
        }
        return false;
    }
    template<class T> bool Read(uintptr_t pointer, T& output) const {
        return Copy(pointer, &output, sizeof(output));
    }
    bool Find(uintptr_t object, size_t root, const char* key, uintptr_t& value) const {
        uintptr_t node = 0;
        if (!Read(object+root, node)) return false;
        const size_t length = std::strlen(key);
        for (unsigned depth=0; node && depth<64; ++depth) {
            unsigned char tag;
            if (!Read(node+0x20, tag)) return false;
            size_t size = tag >> 1;
            uintptr_t data = node+0x21;
            if ((tag&1) && (!Read(node+0x28, size) || !Read(node+0x30, data))) return false;
            char text[256];
            if (size > sizeof(text) || !Copy(data, text, size)) return false;
            int comparison = std::memcmp(key, text, std::min(length, size));
            if (!comparison) comparison = length<size ? -1 : length>size ? 1 : 0;
            if (!comparison) { value=node+0x38; return true; }
            if (!Read(node+(comparison>0 ? 8 : 0), node)) return false;
        }
        return false;
    }
};
// android::sp<T> is a one-pointer nontrivial return, passed via AAPCS64 x8.
// Cleanup is separate so this view's size stays identical to that return ABI.
struct Result {
    void* ptr = nullptr;
    ~Result() {}
};
using StrongFn = void (*)(const void*, const void*);
struct Release {
    void* ptr;
    StrongFn dec;
    ~Release() { if (ptr) dec(ptr, ptr); }
};
} // namespace detail

// Exercise exported constructors with sentinels before inspecting any live
// launcher parameters. This checks the consumed map/string/property layout,
// rather than requiring one ROM build, symbol address, or instruction hash.
inline bool ProbeAnimation(void* gui, void* utils, const char** reason) {
    auto fail = [reason](const char* value) { if (reason) *reason=value; return false; };
    constexpr char kCreate[] =
        "_ZN7android3gui17SfAnimationParams6createENSt3__16vectorIfNS2_9allocatorIfEEEENS3_IiNS4_IiEEEES8_";
    using CreateFn = detail::Result (*)(std::vector<float>, std::vector<int>, std::vector<int>);
    auto create = reinterpret_cast<CreateFn>(dlsym(gui, kCreate));
    auto dec = reinterpret_cast<detail::StrongFn>(dlsym(utils, "_ZNK7android7RefBase9decStrongEPKv"));
    if (!create || !dec) return fail("animation ABI probe symbols unavailable");
    std::vector<float> input;
    for (int i=0; i<8; ++i) {
        input.insert(input.end(), {2.0f, 10.0f+i, 20.0f+i, 0.0f, 30.0f+i, 1.0f, .24f, 0.0f, 0.0f});
    }
    for (int i=0; i<13; ++i) input.insert(input.end(), {-1.0f, 100.0f+i});
    auto result = create(input, {}, {2, 1, 0x12345, 0x23456});
    detail::Release release{result.ptr, dec};
    if (!result.ptr) return fail("animation ABI probe allocation rejected");
    detail::Readable memory;
    const uintptr_t object = reinterpret_cast<uintptr_t>(result.ptr);
    int32_t integers[3]{};
    if (!memory.Copy(object+0x40, integers, sizeof(integers)) ||
        integers[0]!=2 || integers[1]!=1 || integers[2]!=0x12345)
        return fail("animation metadata layout mismatch");
    constexpr const char* keys[] = {"centerX", "centerY", "width", "height", "ratio", "alpha", "radius", "radiusRatio"};
    for (int i=0; i<8; ++i) {
        uintptr_t value=0, property=0;
        float start=0, end=0, velocity=0, damping=0, response=0;
        if (!memory.Find(object, 0x18, keys[i], value) || !memory.Read(value, property) ||
            !memory.Read(property+0x10, start) || !memory.Read(property+0x14, end) ||
            !memory.Read(property+0x1c, velocity) || !memory.Read(property+0x20, damping) ||
            !memory.Read(property+0x24, response) || start!=10.0f+i || end!=20.0f+i ||
            velocity!=30.0f+i || damping!=1.0f || response!=.24f)
            return fail("animation spring/map layout mismatch");
    }
    constexpr const char* fixed_keys[] = {"mainLayerId", "bigIcon", "isNoSourceAnim"};
    constexpr float fixed_values[] = {static_cast<float>(0x23456), 110.0f, 112.0f};
    for (int i=0; i<3; ++i) {
        uintptr_t value=0;
        float fixed=0;
        if (!memory.Find(object, 0x30, fixed_keys[i], value) || !memory.Read(value, fixed) || fixed!=fixed_values[i])
            return fail("animation fixed/map layout mismatch");
    }
    if (reason) *reason=nullptr;
    return true;
}
} // namespace hha::abi
