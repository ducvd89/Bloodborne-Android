// Bloodframe: the half-precision (FP16) shader set of the player's Lossless.dll.
//
// A recent Lossless.dll carries every shader of frame generation three times: as DXBC (resource id N,
// the set the framegen translates to SPIR-V), as SPIR-V for half-precision arithmetic (N + 49) and as
// SPIR-V for full precision (N + 98). This reads the half-precision set, checks it needs nothing the
// framegen's device does not enable, and hands each module over ready to use. Nothing of the dll is
// kept on disk or shipped.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BbHalf {

/// Reads the half-precision SPIR-V of every shader the framegen uses (both models) from the loaded
/// Lossless.dll (Extract::extractShaders) and checks it. Returns an empty string when the set is
/// complete and usable, else why it is not (an old Lossless.dll, a module that needs more than the
/// device features Bloodframe enables).
std::string Prepare();

/// The prepared SPIR-V of a shader (the framegen's own name for it, e.g. "alpha[0]", "p_generate").
/// Throws std::runtime_error when Prepare() did not succeed or the name is unknown.
std::vector<uint8_t> Get(const std::string& name);

/// The SPIR-V capabilities the prepared set uses, e.g. "Float16 ImageQuery Shader ..." (for the log).
std::string Capabilities();

} // namespace BbHalf
