// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ht_plan.h"
#include "image.h"

#include <stdint.h>

namespace hometweaks {

// Full, leased original file-view content; 0 means incomplete/invalid (never cacheable).
// This fingerprint is a cache version discriminator, not an authenticity signature.
uint64_t ImageIdentity(const Image& image);

// Launcher-owned private per-user directory only; cache miss still permits dynamic scanning.
// Outputs publish only after complete validated reads. No shared writable fallback paths.
bool LoadSitesCache(uint64_t imageId, LocatedSites* out);
bool SaveSitesCache(uint64_t imageId, const LocatedSites& sites);

void DropSitesCache();

}
