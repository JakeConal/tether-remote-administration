// lib/protocol/protocol.hpp - umbrella header for the tether protocol v2
// stack (design D3 + D4; tasks 3.1-3.4).
//
// Layers:
//   envelope.hpp  - [ver|type|seq|len] framing codec (wire constants there)
//   payloads.hpp  - JSON payload variant with strict per-type validation
//   bytes.hpp     - IByteSource/IByteSink transport seam + in-memory pipes
//   handshake.hpp - HMAC-SHA256 challenge-response (mbedtls-backed)
//   session.hpp   - console/agent state machines + identity registry
#include "envelope.hpp"
#include "payloads.hpp"
#include "bytes.hpp"
#include "handshake.hpp"
#include "session.hpp"
