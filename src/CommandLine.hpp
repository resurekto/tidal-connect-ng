#pragma once
// Command-line parsing (CLI11). Flag names, defaults and
// value checks follow the original; flags that no longer apply are gone.

#include <iosfwd>

#include "Config.hpp"

namespace tcng {

enum class ParseResult { Run, HelpShown, Error };

// Parses argv into config. Help goes to out, errors to err.
ParseResult parseCommandLine(int argc, const char* const* argv, Config& config,
                             std::ostream& out, std::ostream& err);

}  // namespace tcng
