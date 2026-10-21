#pragma once

#include <string>

namespace ff {

// Loads avutil/avcodec dynamically (Windows). Returns true when all symbols
// are resolved. On failure `*err` carries a human-readable reason.
// Idempotent: subsequent calls are cheap no-ops. On non-Windows builds this
// is a stub that always succeeds (libavcodec is linked normally).
bool load(std::string *err);

} // namespace ff
