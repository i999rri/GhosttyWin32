#include "pch.h"
#include "SEHGuard.h"

extern "C" int RunSEHGuarded(void (*fn)(void*), void* ctx) noexcept {
    __try {
        fn(ctx);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("SEH caught hardware exception inside guarded call\n");
        return 0;
    }
}
