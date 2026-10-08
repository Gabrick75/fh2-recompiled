#pragma once
#include <cstdint>
#include <string_view>

namespace nfsmw::compatibilidad {
// Conservative workaround for tester-reported Mali query driver crashes.
// Covers both game flare and reflection queries, including pool creation/reset.
inline bool OclusionGpu(std::string_view modo, bool android, uint32_t vendor) {
  if (modo == "off") return false;
  if (modo == "on") return true;
  return !(android && vendor == 0x13B5);  // Arm
}
}  // namespace nfsmw::compatibilidad
