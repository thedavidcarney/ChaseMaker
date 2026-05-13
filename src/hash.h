// FNV-1a 32-bit string hash. Bit-for-bit compatible with EXRDemux's
// HashLayerName (src/exrdemux.cpp) so a layer's hash computed here
// can be stored in EXRDemux's Layer Hash Hi / Lo float-slider params
// and resolved back to the same display name at render time.
//
// Also matches the JSX implementation in EXRDemux's
// scripts/SplitAndSortPassesToPrecomps.jsx — the contract that
// makes name-based selection survive re-renders.

#pragma once

#include <cstdint>
#include <string>

uint32_t FNV1a32(const std::string& s);
