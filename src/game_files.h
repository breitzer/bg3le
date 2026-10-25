#pragma once

// Reading the game's own data files -- what upstream reaches through the
// engine's virtual file system.
//
// Upstream opens these with StaticSymbols::MakeFileReader, which needs
// ls::FileReader's constructor, and no engine function in the Linux build
// carries a symbol to call. So bg3le reads the archives itself: the index is
// built once from every LSPK under the install, and a loose file on disk
// wins over a packed one the way it does for the engine.

#include <string>
#include <vector>

namespace bg3le {

// The install's Data directory, derived from the running executable.
std::string const& game_data_root();

// The contents of a Data-relative path, e.g.
// "Public/Game/GUI/Assets/Fonts/QuadraatOffcPro/QuadraatOffcPro.ttf".
//
// Refuses an absolute path or one containing traversal. The first call
// indexes every archive under the install, which takes tens of
// milliseconds; after that a lookup is a map probe and one decompression.
bool game_file_read(char const* relative, std::string* out);

// Whether the path exists, without reading it.
bool game_file_exists(char const* relative);

// Every packed path that starts with prefix, in path order.
std::vector<std::string> game_files_under(std::string const& prefix);

// A file under Mods/ inside an installed mod's archive (src/ext_libs.cpp).
bool mod_file_read(char const* relative, std::string* out);

}  // namespace bg3le
