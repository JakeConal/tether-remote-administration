// Translation-unit anchor for tether-protocol (Phase 2 scaffolding).
// The real protocol v2 codec (envelope [ver | type | seq | len] + JSON
// payloads) replaces this in Phase 3 (tasks 3.1-3.5, design D3).
#include "protocol.hpp"

namespace tether::protocol {

unsigned placeholder_anchor() { return kEnvelopeVersion; }

} // namespace tether::protocol
