// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace FEX::Windows::Logging {
void Init();
void SetAddressTranslator(uint64_t (*Fn)(uint64_t));
} // namespace FEX::Windows::Logging
