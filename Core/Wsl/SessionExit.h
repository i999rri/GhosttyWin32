#pragma once

// How an in-place WSL session (#217) ended: the exit code and how long
// the process lived, which arrive together and are only useful
// together.

#include <cstdint>

namespace core::wsl {

// There is deliberately no method per exit code. The codes have no
// agreed meaning to name one after: 127 is a shell's convention for a
// command it could not find, 4294967295 is wsl.exe's -1, and any
// program is free to return either for reasons of its own. A predicate
// called IsHelperMissing would be a guess presented as a fact.
//
// So the code is carried, not read. It goes to the shim, where it
// becomes the exit code of the `wsl` the user typed, and the one thing
// the host decides is asked of both numbers at once.
class SessionExit {
public:
    SessionExit(uint32_t code, uint64_t livedMs) noexcept
        : m_code(code), m_livedMs(livedMs) {}

    uint32_t Code() const noexcept { return m_code; }

    // Whether the session never got as far as being usable. Such a pane
    // has its reason written on it and ghostty keeps it open rather than
    // closing it (Surface.childExited), so the host leaves the shell
    // waiting instead of restoring it over the answer.
    //
    // `abnormalRuntimeMs` is ghostty's abnormal-command-exit-runtime,
    // read from the config rather than chosen here: a different answer
    // would restore the shell over a pane ghostty kept.
    //
    // The code alone cannot say this. `exit 3` is non-zero and ends a
    // session that was used. Nor can it say who ended the session:
    // someone fast enough to type `exit 3` within the window gets their
    // pane kept, which costs one keypress. Guessing from the code
    // instead would cost correctness in every case it guessed wrong.
    bool FailedBeforeUse(uint32_t abnormalRuntimeMs) const noexcept {
        return m_code != 0 && m_livedMs <= abnormalRuntimeMs;
    }

private:
    uint32_t m_code;
    uint64_t m_livedMs;
};

}  // namespace core::wsl
