#pragma once

#include "options.h"

namespace tc {

// Runs the whole job. Returns the process exit code:
// 0 = success, 1 = setup/usage error, 2 = finished but some entries failed.
int run(const Options& opt);

} // namespace tc
