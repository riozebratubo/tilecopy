#include "engine.h"
#include "fsmeta.h"
#include "options.h"

#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    auto opt = tc::parse_command_line(argc, argv);
    if (!opt) return 1;

    tc::enable_backup_privileges();
    return tc::run(*opt);
}
