#pragma once

#include "common/util/FileUtil.h"

namespace decompiler {
// PC-REMAKE : cartes du rivage des niveaux cotiers (voir shore_map.cpp), a partir de l'export
// de l'editeur de niveaux ; ecrites dans out_dir/<niveau>.riv
void build_shore_maps(const fs::path& editor_dir, const fs::path& out_dir);
}  // namespace decompiler
