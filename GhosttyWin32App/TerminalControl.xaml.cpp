#include "pch.h"
#include "TerminalControl.xaml.h"
#if __has_include("TerminalControl.g.cpp")
#include "TerminalControl.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    TerminalControl::TerminalControl()
    {
        InitializeComponent();
    }
}
