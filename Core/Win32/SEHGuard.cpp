#include "Win32/SEHGuard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" int RunSEHGuarded(void (*fn)(void*), void* ctx) noexcept {
    __try {
        fn(ctx);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("SEH caught hardware exception inside guarded call\n");
        return 0;
    }
}
