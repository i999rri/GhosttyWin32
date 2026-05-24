#pragma once

// SEH "trampoline": isolates a callable invocation behind __try/__except.
// MSVC's /EHsc refuses __try in any function that has C++ unwinding
// (i.e. anything dealing with C++ objects), so the unsafe call has to
// be reached through a function pointer from a frame that holds only
// raw C types. The C++ work lives in the callback we invoke through
// `fn` — if that callback raises a hardware exception (AV, illegal
// instruction, etc.) it's swallowed here and `0` is returned.
//
// Use case: wrap calls into known-buggy XAML internals (e.g. rapid-
// teardown SetSwapChainHandle) or third-party drivers (NVIDIA
// Present-time AVs) where the alternative is the whole process going
// down for an exception we have no way to fix.
//
// Returns 1 on normal completion, 0 if the SEH handler caught a
// hardware exception. Either way the caller proceeds — the trampoline
// itself never throws.
extern "C" int RunSEHGuarded(void (*fn)(void*), void* ctx) noexcept;
