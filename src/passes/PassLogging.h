#pragma once

#include <iostream>

namespace PassLogging {
// Mirrors ModularRenderer's default: compile-time verbose logging disabled.
inline constexpr bool VerboseLogging = false;
}

#define PASS_VERBOSE_LOG(runtimeEnabled, messageExpr)                                   \
    do {                                                                                 \
        if constexpr (PassLogging::VerboseLogging) {                                     \
            if (runtimeEnabled) {                                                        \
                std::cout << messageExpr << std::endl;                                   \
            }                                                                            \
        }                                                                                \
    } while (0)
