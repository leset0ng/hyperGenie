// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <android/log.h>
#include <sys/system_properties.h>
#include <cstdio>

namespace hha {
// Device, fingerprint, launcher release and OS minor version are not ABI gates.
// Required native interfaces and layouts are checked separately before use.
inline bool CheckHyperOS(const char* tag) {
    char version[PROP_VALUE_MAX]{};
    __system_property_get("ro.mi.os.version.name", version);
    int major = 0;
    if (sizeof(void*) != 8 || std::sscanf(version, "OS%d", &major) != 1 || major < 4) {
        __android_log_print(ANDROID_LOG_WARN, tag,
            "HyperOS 4+ on arm64 required; keeping stock (version=%s)", version);
        return false;
    }
    return true;
}
}  // namespace hha
