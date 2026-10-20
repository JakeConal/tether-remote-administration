// lib/protocol - placeholder header for tether-protocol (Phase 2 scaffolding).
// The real protocol v2 codec replaces this in Phase 3 (tasks 3.1-3.5):
// envelope [ver | type | seq | len] + JSON payloads (design D3).
#pragma once

namespace tether::protocol {

// Envelope version carried in every message's 1-byte ver field (design D3).
inline constexpr unsigned char kEnvelopeVersion = 2;

// Translation-unit anchor; removed when the codec lands in Phase 3.
unsigned placeholder_anchor();

} // namespace tether::protocol
