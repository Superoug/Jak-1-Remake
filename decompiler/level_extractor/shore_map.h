#pragma once

#include "common/util/FileUtil.h"

namespace decompiler {
// cartes du rivage des niveaux cotiers (voir shore_map.cpp)
void build_shore_maps(const fs::path& editor_dir, const fs::path& out_dir);
}  // namespace decompiler
