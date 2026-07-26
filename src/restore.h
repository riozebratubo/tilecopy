#pragma once

#include "options.h"

namespace tc {

// Writes a .vhdx made by --drive or --partition back onto real hardware
// (--restore): a whole-disk image onto a physical disk, a volume image onto a
// volume. Exit codes as elsewhere: 0 = success, 1 = setup error or the user
// declined, 2 = some chunks failed or did not match the recorded hashes.
int run_restore(const Options& opt);

} // namespace tc
