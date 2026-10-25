#pragma once

#include <utility>
#include <variant>

namespace core::input {

namespace detail {
// One call operator built from several, told apart by parameter type.
template <class... Fs> struct Overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;
}  // namespace detail

// What the host does with a mouse press. Decided from the host's own
// state and handed back as a value, so the deciding and the doing are
// two places: whoever holds one calls Match with a handler per case.
//
// The cases are types, not enumerators, so that Match tells the
// handlers apart by their parameter: a case left without a handler
// fails to compile, and the order they are written in is free.
class PressAction {
public:
    // libghostty has already heard this press.
    struct Ignore {};
    // The right-click gesture: copy what is selected. libghostty hears
    // nothing.
    struct CopySelection {};
    // Record it, hold the capture, tell libghostty.
    struct RelayToTerminal {};

    constexpr PressAction(Ignore) noexcept : m_value(Ignore{}) {}
    constexpr PressAction(CopySelection) noexcept : m_value(CopySelection{}) {}
    constexpr PressAction(RelayToTerminal) noexcept : m_value(RelayToTerminal{}) {}

    template <class... Handlers>
    constexpr decltype(auto) Match(Handlers&&... handlers) const {
        return std::visit(detail::Overloaded{ std::forward<Handlers>(handlers)... }, m_value);
    }

private:
    std::variant<Ignore, CopySelection, RelayToTerminal> m_value;
};

}  // namespace core::input
