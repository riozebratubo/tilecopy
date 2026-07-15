#pragma once

#include "options.h"

namespace tc {

// Raw image copy of a whole physical disk (--drive to a .vhdx) or a single
// volume wrapped in a GPT (--partition). Same exit codes as run():
// 0 = success, 1 = setup error, 2 = some chunks failed or the DB not saved.
int run_image(const Options& opt);

} // namespace tc
