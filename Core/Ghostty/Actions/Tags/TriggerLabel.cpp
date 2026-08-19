#include "TriggerLabel.h"

namespace core::ghostty::actions {

namespace {

// Generated from the ghostty_input_key_e enum in
// external/ghostty/include/ghostty.h (GHOSTTY_KEY_X -> "x").
// Regenerate mechanically if the submodule adds keys; an unknown
// value falls back to "unidentified" rather than breaking.
const wchar_t* KeyName(ghostty_input_key_e key) {
    switch (key) {
        case GHOSTTY_KEY_UNIDENTIFIED: return L"unidentified";
        case GHOSTTY_KEY_BACKQUOTE: return L"backquote";
        case GHOSTTY_KEY_BACKSLASH: return L"backslash";
        case GHOSTTY_KEY_BRACKET_LEFT: return L"bracket_left";
        case GHOSTTY_KEY_BRACKET_RIGHT: return L"bracket_right";
        case GHOSTTY_KEY_COMMA: return L"comma";
        case GHOSTTY_KEY_DIGIT_0: return L"digit_0";
        case GHOSTTY_KEY_DIGIT_1: return L"digit_1";
        case GHOSTTY_KEY_DIGIT_2: return L"digit_2";
        case GHOSTTY_KEY_DIGIT_3: return L"digit_3";
        case GHOSTTY_KEY_DIGIT_4: return L"digit_4";
        case GHOSTTY_KEY_DIGIT_5: return L"digit_5";
        case GHOSTTY_KEY_DIGIT_6: return L"digit_6";
        case GHOSTTY_KEY_DIGIT_7: return L"digit_7";
        case GHOSTTY_KEY_DIGIT_8: return L"digit_8";
        case GHOSTTY_KEY_DIGIT_9: return L"digit_9";
        case GHOSTTY_KEY_EQUAL: return L"equal";
        case GHOSTTY_KEY_INTL_BACKSLASH: return L"intl_backslash";
        case GHOSTTY_KEY_INTL_RO: return L"intl_ro";
        case GHOSTTY_KEY_INTL_YEN: return L"intl_yen";
        case GHOSTTY_KEY_A: return L"a";
        case GHOSTTY_KEY_B: return L"b";
        case GHOSTTY_KEY_C: return L"c";
        case GHOSTTY_KEY_D: return L"d";
        case GHOSTTY_KEY_E: return L"e";
        case GHOSTTY_KEY_F: return L"f";
        case GHOSTTY_KEY_G: return L"g";
        case GHOSTTY_KEY_H: return L"h";
        case GHOSTTY_KEY_I: return L"i";
        case GHOSTTY_KEY_J: return L"j";
        case GHOSTTY_KEY_K: return L"k";
        case GHOSTTY_KEY_L: return L"l";
        case GHOSTTY_KEY_M: return L"m";
        case GHOSTTY_KEY_N: return L"n";
        case GHOSTTY_KEY_O: return L"o";
        case GHOSTTY_KEY_P: return L"p";
        case GHOSTTY_KEY_Q: return L"q";
        case GHOSTTY_KEY_R: return L"r";
        case GHOSTTY_KEY_S: return L"s";
        case GHOSTTY_KEY_T: return L"t";
        case GHOSTTY_KEY_U: return L"u";
        case GHOSTTY_KEY_V: return L"v";
        case GHOSTTY_KEY_W: return L"w";
        case GHOSTTY_KEY_X: return L"x";
        case GHOSTTY_KEY_Y: return L"y";
        case GHOSTTY_KEY_Z: return L"z";
        case GHOSTTY_KEY_MINUS: return L"minus";
        case GHOSTTY_KEY_PERIOD: return L"period";
        case GHOSTTY_KEY_QUOTE: return L"quote";
        case GHOSTTY_KEY_SEMICOLON: return L"semicolon";
        case GHOSTTY_KEY_SLASH: return L"slash";
        case GHOSTTY_KEY_ALT_LEFT: return L"alt_left";
        case GHOSTTY_KEY_ALT_RIGHT: return L"alt_right";
        case GHOSTTY_KEY_BACKSPACE: return L"backspace";
        case GHOSTTY_KEY_CAPS_LOCK: return L"caps_lock";
        case GHOSTTY_KEY_CONTEXT_MENU: return L"context_menu";
        case GHOSTTY_KEY_CONTROL_LEFT: return L"control_left";
        case GHOSTTY_KEY_CONTROL_RIGHT: return L"control_right";
        case GHOSTTY_KEY_ENTER: return L"enter";
        case GHOSTTY_KEY_META_LEFT: return L"meta_left";
        case GHOSTTY_KEY_META_RIGHT: return L"meta_right";
        case GHOSTTY_KEY_SHIFT_LEFT: return L"shift_left";
        case GHOSTTY_KEY_SHIFT_RIGHT: return L"shift_right";
        case GHOSTTY_KEY_SPACE: return L"space";
        case GHOSTTY_KEY_TAB: return L"tab";
        case GHOSTTY_KEY_CONVERT: return L"convert";
        case GHOSTTY_KEY_KANA_MODE: return L"kana_mode";
        case GHOSTTY_KEY_NON_CONVERT: return L"non_convert";
        case GHOSTTY_KEY_DELETE: return L"delete";
        case GHOSTTY_KEY_END: return L"end";
        case GHOSTTY_KEY_HELP: return L"help";
        case GHOSTTY_KEY_HOME: return L"home";
        case GHOSTTY_KEY_INSERT: return L"insert";
        case GHOSTTY_KEY_PAGE_DOWN: return L"page_down";
        case GHOSTTY_KEY_PAGE_UP: return L"page_up";
        case GHOSTTY_KEY_ARROW_DOWN: return L"arrow_down";
        case GHOSTTY_KEY_ARROW_LEFT: return L"arrow_left";
        case GHOSTTY_KEY_ARROW_RIGHT: return L"arrow_right";
        case GHOSTTY_KEY_ARROW_UP: return L"arrow_up";
        case GHOSTTY_KEY_NUM_LOCK: return L"num_lock";
        case GHOSTTY_KEY_NUMPAD_0: return L"numpad_0";
        case GHOSTTY_KEY_NUMPAD_1: return L"numpad_1";
        case GHOSTTY_KEY_NUMPAD_2: return L"numpad_2";
        case GHOSTTY_KEY_NUMPAD_3: return L"numpad_3";
        case GHOSTTY_KEY_NUMPAD_4: return L"numpad_4";
        case GHOSTTY_KEY_NUMPAD_5: return L"numpad_5";
        case GHOSTTY_KEY_NUMPAD_6: return L"numpad_6";
        case GHOSTTY_KEY_NUMPAD_7: return L"numpad_7";
        case GHOSTTY_KEY_NUMPAD_8: return L"numpad_8";
        case GHOSTTY_KEY_NUMPAD_9: return L"numpad_9";
        case GHOSTTY_KEY_NUMPAD_ADD: return L"numpad_add";
        case GHOSTTY_KEY_NUMPAD_BACKSPACE: return L"numpad_backspace";
        case GHOSTTY_KEY_NUMPAD_CLEAR: return L"numpad_clear";
        case GHOSTTY_KEY_NUMPAD_CLEAR_ENTRY: return L"numpad_clear_entry";
        case GHOSTTY_KEY_NUMPAD_COMMA: return L"numpad_comma";
        case GHOSTTY_KEY_NUMPAD_DECIMAL: return L"numpad_decimal";
        case GHOSTTY_KEY_NUMPAD_DIVIDE: return L"numpad_divide";
        case GHOSTTY_KEY_NUMPAD_ENTER: return L"numpad_enter";
        case GHOSTTY_KEY_NUMPAD_EQUAL: return L"numpad_equal";
        case GHOSTTY_KEY_NUMPAD_MEMORY_ADD: return L"numpad_memory_add";
        case GHOSTTY_KEY_NUMPAD_MEMORY_CLEAR: return L"numpad_memory_clear";
        case GHOSTTY_KEY_NUMPAD_MEMORY_RECALL: return L"numpad_memory_recall";
        case GHOSTTY_KEY_NUMPAD_MEMORY_STORE: return L"numpad_memory_store";
        case GHOSTTY_KEY_NUMPAD_MEMORY_SUBTRACT: return L"numpad_memory_subtract";
        case GHOSTTY_KEY_NUMPAD_MULTIPLY: return L"numpad_multiply";
        case GHOSTTY_KEY_NUMPAD_PAREN_LEFT: return L"numpad_paren_left";
        case GHOSTTY_KEY_NUMPAD_PAREN_RIGHT: return L"numpad_paren_right";
        case GHOSTTY_KEY_NUMPAD_SUBTRACT: return L"numpad_subtract";
        case GHOSTTY_KEY_NUMPAD_SEPARATOR: return L"numpad_separator";
        case GHOSTTY_KEY_NUMPAD_UP: return L"numpad_up";
        case GHOSTTY_KEY_NUMPAD_DOWN: return L"numpad_down";
        case GHOSTTY_KEY_NUMPAD_RIGHT: return L"numpad_right";
        case GHOSTTY_KEY_NUMPAD_LEFT: return L"numpad_left";
        case GHOSTTY_KEY_NUMPAD_BEGIN: return L"numpad_begin";
        case GHOSTTY_KEY_NUMPAD_HOME: return L"numpad_home";
        case GHOSTTY_KEY_NUMPAD_END: return L"numpad_end";
        case GHOSTTY_KEY_NUMPAD_INSERT: return L"numpad_insert";
        case GHOSTTY_KEY_NUMPAD_DELETE: return L"numpad_delete";
        case GHOSTTY_KEY_NUMPAD_PAGE_UP: return L"numpad_page_up";
        case GHOSTTY_KEY_NUMPAD_PAGE_DOWN: return L"numpad_page_down";
        case GHOSTTY_KEY_ESCAPE: return L"escape";
        case GHOSTTY_KEY_F1: return L"f1";
        case GHOSTTY_KEY_F2: return L"f2";
        case GHOSTTY_KEY_F3: return L"f3";
        case GHOSTTY_KEY_F4: return L"f4";
        case GHOSTTY_KEY_F5: return L"f5";
        case GHOSTTY_KEY_F6: return L"f6";
        case GHOSTTY_KEY_F7: return L"f7";
        case GHOSTTY_KEY_F8: return L"f8";
        case GHOSTTY_KEY_F9: return L"f9";
        case GHOSTTY_KEY_F10: return L"f10";
        case GHOSTTY_KEY_F11: return L"f11";
        case GHOSTTY_KEY_F12: return L"f12";
        case GHOSTTY_KEY_F13: return L"f13";
        case GHOSTTY_KEY_F14: return L"f14";
        case GHOSTTY_KEY_F15: return L"f15";
        case GHOSTTY_KEY_F16: return L"f16";
        case GHOSTTY_KEY_F17: return L"f17";
        case GHOSTTY_KEY_F18: return L"f18";
        case GHOSTTY_KEY_F19: return L"f19";
        case GHOSTTY_KEY_F20: return L"f20";
        case GHOSTTY_KEY_F21: return L"f21";
        case GHOSTTY_KEY_F22: return L"f22";
        case GHOSTTY_KEY_F23: return L"f23";
        case GHOSTTY_KEY_F24: return L"f24";
        case GHOSTTY_KEY_F25: return L"f25";
        case GHOSTTY_KEY_FN: return L"fn";
        case GHOSTTY_KEY_FN_LOCK: return L"fn_lock";
        case GHOSTTY_KEY_PRINT_SCREEN: return L"print_screen";
        case GHOSTTY_KEY_SCROLL_LOCK: return L"scroll_lock";
        case GHOSTTY_KEY_PAUSE: return L"pause";
        case GHOSTTY_KEY_BROWSER_BACK: return L"browser_back";
        case GHOSTTY_KEY_BROWSER_FAVORITES: return L"browser_favorites";
        case GHOSTTY_KEY_BROWSER_FORWARD: return L"browser_forward";
        case GHOSTTY_KEY_BROWSER_HOME: return L"browser_home";
        case GHOSTTY_KEY_BROWSER_REFRESH: return L"browser_refresh";
        case GHOSTTY_KEY_BROWSER_SEARCH: return L"browser_search";
        case GHOSTTY_KEY_BROWSER_STOP: return L"browser_stop";
        case GHOSTTY_KEY_EJECT: return L"eject";
        case GHOSTTY_KEY_LAUNCH_APP_1: return L"launch_app_1";
        case GHOSTTY_KEY_LAUNCH_APP_2: return L"launch_app_2";
        case GHOSTTY_KEY_LAUNCH_MAIL: return L"launch_mail";
        case GHOSTTY_KEY_MEDIA_PLAY_PAUSE: return L"media_play_pause";
        case GHOSTTY_KEY_MEDIA_SELECT: return L"media_select";
        case GHOSTTY_KEY_MEDIA_STOP: return L"media_stop";
        case GHOSTTY_KEY_MEDIA_TRACK_NEXT: return L"media_track_next";
        case GHOSTTY_KEY_MEDIA_TRACK_PREVIOUS: return L"media_track_previous";
        case GHOSTTY_KEY_POWER: return L"power";
        case GHOSTTY_KEY_SLEEP: return L"sleep";
        case GHOSTTY_KEY_AUDIO_VOLUME_DOWN: return L"audio_volume_down";
        case GHOSTTY_KEY_AUDIO_VOLUME_MUTE: return L"audio_volume_mute";
        case GHOSTTY_KEY_AUDIO_VOLUME_UP: return L"audio_volume_up";
        case GHOSTTY_KEY_WAKE_UP: return L"wake_up";
        case GHOSTTY_KEY_COPY: return L"copy";
        case GHOSTTY_KEY_CUT: return L"cut";
        case GHOSTTY_KEY_PASTE: return L"paste";
        default: return L"unidentified";
    }
}

// Append the UTF-16 encoding of a Unicode codepoint (keybinds can
// name astral-plane characters, so surrogate pairs are handled).
void AppendCodepoint(std::wstring& out, uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<wchar_t>(cp));
        return;
    }
    cp -= 0x10000;
    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
}

}  // namespace

std::wstring TriggerLabel(ghostty_input_trigger_s trigger) {
    std::wstring out;
    // Same modifier order ghostty's docs use in keybind examples.
    if (trigger.mods & GHOSTTY_MODS_CTRL)  out += L"ctrl+";
    if (trigger.mods & GHOSTTY_MODS_ALT)   out += L"alt+";
    if (trigger.mods & GHOSTTY_MODS_SHIFT) out += L"shift+";
    if (trigger.mods & GHOSTTY_MODS_SUPER) out += L"super+";
    switch (trigger.tag) {
        case GHOSTTY_TRIGGER_PHYSICAL:
            // "physical:" prefix mirrors the config syntax for
            // scancode-addressed bindings.
            out += L"physical:";
            out += KeyName(trigger.key.physical);
            break;
        case GHOSTTY_TRIGGER_UNICODE:
            AppendCodepoint(out, trigger.key.unicode);
            break;
        case GHOSTTY_TRIGGER_CATCH_ALL:
            out += L"any";
            break;
    }
    return out;
}

}  // namespace core::ghostty::actions
