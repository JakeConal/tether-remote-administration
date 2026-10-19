// lib/protocol - placeholder header (Phase 1 skeleton).
// The real protocol v2 codec replaces this in Phase 3:
// envelope [ver | type | seq | len] + JSON payloads (design D3/D8).
#pragma once

namespace tether::protocol {

// Envelope version carried in every message's 1-byte ver field (design D3).
inline constexpr unsigned char kEnvelopeVersion = 2;

} // namespace tether::protocol
