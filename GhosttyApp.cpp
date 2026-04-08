#include "pch.h"
#include "GhosttyApp.h"
#include <cstdio>

#ifdef _DEBUG
#define DBG_LOG(msg) OutputDebugStringA(msg)
#else
#define DBG_LOG(msg) ((void)0)
#endif

// DirectX renderer: notify resize from main thread (exported from ghostty.dll)
extern "C" void dx_notify_resize(uint32_t w, uint32_t h);

bool GhosttyApp::initialize() {
    if (m_initialized) return true;

    // Run ghostty_init + config on 4MB stack thread (max_path_bytes stack overflow workaround)
    char arg0[] = "ghostty";
    char* argv[] = { arg0 };

    struct InitArgs {
        int argc; char** argv;
        int result;
        ghostty_config_t config;
    };
    InitArgs initArgs = { 1, argv, -1, nullptr };

    HANDLE hThread = CreateThread(
        nullptr, 4 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* args = static_cast<InitArgs*>(param);
            args->result = ghostty_init(args->argc, args->argv);
            if (args->result != 0) return 0;

            args->config = ghostty_config_new();
            if (!args->config) return 0;

            char configPath[MAX_PATH] = {};
            if (GetEnvironmentVariableA("LOCALAPPDATA", configPath, MAX_PATH)) {
                strcat_s(configPath, "\\ghostty\\config");
                DWORD attr = GetFileAttributesA(configPath);
                if (attr != INVALID_FILE_ATTRIBUTES) {
                    ghostty_config_load_file(args->config, configPath);
                }
            }
            ghostty_config_finalize(args->config);
            return 0;
        },
        &initArgs, 0, nullptr);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    if (initArgs.result != 0) {
        DBG_LOG("ghostty: init failed\n");
        return false;
    }

    m_config = initArgs.config;
    if (!m_config) {
        DBG_LOG("ghostty: config creation failed\n");
        return false;
    }

    // Log config diagnostics
    uint32_t diagCount = ghostty_config_diagnostics_count(m_config);
    for (uint32_t i = 0; i < diagCount; i++) {
        ghostty_diagnostic_s diag = ghostty_config_get_diagnostic(m_config, i);
        if (diag.message) {
            OutputDebugStringA("ghostty: config diag: ");
            OutputDebugStringA(diag.message);
            OutputDebugStringA("\n");
        }
    }

    // Create app with runtime callbacks
    ghostty_runtime_config_s rtConfig = {};
    rtConfig.userdata = this;
    rtConfig.supports_selection_clipboard = false;
    rtConfig.wakeup_cb = &GhosttyApp::onWakeup;
    rtConfig.action_cb = &GhosttyApp::onAction;
    rtConfig.read_clipboard_cb = &GhosttyApp::onReadClipboard;
    rtConfig.confirm_read_clipboard_cb = &GhosttyApp::onConfirmReadClipboard;
    rtConfig.write_clipboard_cb = &GhosttyApp::onWriteClipboard;
    rtConfig.close_surface_cb = &GhosttyApp::onCloseSurface;

    m_app = ghostty_app_new(&rtConfig, m_config);
    if (!m_app) {
        DBG_LOG("ghostty: app creation failed\n");
        ghostty_config_free(m_config);
        m_config = nullptr;
        return false;
    }

    DBG_LOG("ghostty: app created!\n");
    m_initialized = true;
    return true;
}

void GhosttyApp::shutdown() {
    if (m_app) {
        ghostty_app_free(m_app);
        m_app = nullptr;
    }
    m_config = nullptr;
    m_initialized = false;
}

ghostty_surface_t GhosttyApp::createSurface(HWND hwnd) {
    if (!m_initialized || !m_app || !hwnd) return nullptr;

    struct Args {
        GhosttyApp* self;
        HWND hwnd;
        ghostty_surface_t result;
    };
    Args args = { this, hwnd, nullptr };

    HANDLE hThread = CreateThread(
        nullptr, 4 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* a = static_cast<Args*>(param);

            ghostty_surface_config_s surfConfig = ghostty_surface_config_new();
            surfConfig.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
            surfConfig.platform.windows.hwnd = a->hwnd;
            surfConfig.platform.windows.hdc = nullptr;
            surfConfig.platform.windows.hglrc = nullptr;

            UINT dpi = GetDpiForWindow(a->hwnd);
            surfConfig.scale_factor = (double)dpi / 96.0;

            a->result = ghostty_surface_new(a->self->m_app, &surfConfig);
            return 0;
        },
        &args, 0, nullptr);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    if (args.result) {
        DBG_LOG("ghostty: surface created!\n");
    } else {
        DBG_LOG("ghostty: surface creation failed\n");
    }
    return args.result;
}

void GhosttyApp::destroySurface(ghostty_surface_t surface) {
    if (surface) {
        ghostty_surface_free(surface);
        DBG_LOG("ghostty: surface destroyed\n");
    }
}

// --- Callbacks ---

void GhosttyApp::onWakeup(void* userdata) {
    auto* self = static_cast<GhosttyApp*>(userdata);
    if (self && self->onWakeupFn) self->onWakeupFn();
}

bool GhosttyApp::onAction(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action) {
    auto* self = static_cast<GhosttyApp*>(ghostty_app_userdata(app));
    if (self && self->onActionFn) return self->onActionFn(target, action);
    (void)target;
    return false;
}

bool GhosttyApp::onReadClipboard(void* /*userdata*/, ghostty_clipboard_e /*clipboard*/, void* /*state*/) {
    // TODO: implement via WinUI clipboard API
    return false;
}

void GhosttyApp::onConfirmReadClipboard(void* /*userdata*/, const char* /*content*/, void* /*state*/, ghostty_clipboard_request_e /*req*/) {
    // TODO
}

void GhosttyApp::onWriteClipboard(void* /*userdata*/, ghostty_clipboard_e /*clipboard*/, const ghostty_clipboard_content_s* /*content*/, size_t /*count*/, bool /*confirm*/) {
    // TODO: implement via WinUI clipboard API
}

void GhosttyApp::onCloseSurface(void* userdata, bool process_exited) {
    auto* self = static_cast<GhosttyApp*>(userdata);
    if (self && self->onCloseSurfaceFn) {
        // TODO: need to identify which surface closed
        self->onCloseSurfaceFn(nullptr, process_exited);
    }
}
