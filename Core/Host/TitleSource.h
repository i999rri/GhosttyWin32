#pragma once

namespace core::host {

// Who last named a tab — and therefore who may rename it now.
//
// Three writers compete for a tab's header: the foreground-pid poll
// (automatic process names), the shell (SET_TITLE / SET_TAB_TITLE,
// i.e. OSC 0/2 or the `set_title` action), and the user (the rename
// prompt, PROMPT_TITLE). Upstream documents a prompt-set title as
// overriding any title set by the terminal, and a shell title as
// winning over the computed one — a strict priority:
//
//   Automatic < Shell < User
//
// which collapses to one rule at every write site: a writer may set
// the header only when the current source does not outrank it. Equal
// rank re-asserts freely (the shell re-sends its OSC title on every
// prompt, the poll refreshes its own name on process changes) — that
// is what lets a title ever change at all.
class TitleSource {
public:
    static constexpr TitleSource Automatic() noexcept { return TitleSource(Kind::Automatic); }
    static constexpr TitleSource Shell() noexcept { return TitleSource(Kind::Shell); }
    static constexpr TitleSource User() noexcept { return TitleSource(Kind::User); }

    constexpr bool IsAutomatic() const noexcept { return m_kind == Kind::Automatic; }
    constexpr bool IsShell() const noexcept { return m_kind == Kind::Shell; }
    constexpr bool IsUser() const noexcept { return m_kind == Kind::User; }

    // The write rule above: true when a title from this source must
    // not be overwritten by `writer`.
    constexpr bool Outranks(TitleSource writer) const noexcept {
        return static_cast<int>(m_kind) > static_cast<int>(writer.m_kind);
    }

    constexpr bool operator==(TitleSource const&) const noexcept = default;

private:
    // Enumerator order is the priority order — Outranks compares it.
    enum class Kind { Automatic, Shell, User };

    constexpr explicit TitleSource(Kind kind) noexcept : m_kind(kind) {}

    Kind m_kind;
};

}  // namespace core::host
