#pragma once

#include "ghostty.h"
#include <memory>
#include <windows.h>

namespace winrt::GhosttyWin32::implementation {

// RAII wrapper around the global ghostty_app_t + ghostty_config_t pair.
// Encapsulates the 4MB-stack worker thread that ghostty_init / ghostty_app_new
// require, and frees both handles in the right order on destruction.
//
// Construction is via Create() because the init dance can fail and we want
// the caller to see nullptr instead of a half-built object. The runtime
// config (callbacks + userdata) is supplied by the caller — typically wired
// to the host's MainWindow — and copied into ghostty_app_new before the
// caller's reference goes out of scope.
class GhosttyApp {
public:
    GhosttyApp(GhosttyApp const&) = delete;
    GhosttyApp& operator=(GhosttyApp const&) = delete;
    GhosttyApp(GhosttyApp&&) = delete;
    GhosttyApp& operator=(GhosttyApp&&) = delete;

    static std::unique_ptr<GhosttyApp> Create(ghostty_runtime_config_s rtConfig) {
        // Bundle the work for the 4MB-stack thread. ghostty_init / ghostty_app_new
        // run deep enough Zig code that the default 1MB thread stack overflows.
        struct Ctx {
            ghostty_runtime_config_s rtConfig;
            ghostty_app_t app{ nullptr };
            ghostty_config_t config{ nullptr };
        };
        Ctx ctx{ rtConfig, nullptr, nullptr };

        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* c = static_cast<Ctx*>(param);
                ghostty_init(0, nullptr);
                c->config = ghostty_config_new();
                if (c->config) {
                    ghostty_config_load_default_files(c->config);
                    ghostty_config_finalize(c->config);
                    c->app = ghostty_app_new(&c->rtConfig, c->config);
                }
                return 0;
            }, &ctx, 0, nullptr);

        if (!hThread) return nullptr;
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);

        if (!ctx.app) {
            if (ctx.config) ghostty_config_free(ctx.config);
            return nullptr;
        }
        return std::unique_ptr<GhosttyApp>(new GhosttyApp(ctx.app, ctx.config));
    }

    ~GhosttyApp() {
        // ghostty_app_free joins surface/IO threads internally and must run
        // before ghostty_config_free — the app holds a reference to config.
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
    }

    ghostty_app_t Handle() const noexcept { return m_app; }
    void Tick() noexcept { if (m_app) ghostty_app_tick(m_app); }

private:
    GhosttyApp(ghostty_app_t app, ghostty_config_t config) noexcept
        : m_app(app), m_config(config) {}

    ghostty_app_t m_app{ nullptr };
    ghostty_config_t m_config{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
