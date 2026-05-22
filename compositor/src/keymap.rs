//! Android `KeyEvent.KEYCODE_*` → Linux evdev keycode mapping.
//!
//! `nativeSendKeyEvent` from Kotlin hands us Android KeyEvent codes;
//! `KeyboardHandle::input` wants evdev keycodes (event_loop offsets by +8
//! to land in XKB space). All translation lives here so the JNI entry
//! point stays a thin shim.
//!
//! The numeric codes on both sides come from:
//! - Android: `frameworks/base/core/java/android/view/KeyEvent.java`
//! - Linux:   `/usr/include/linux/input-event-codes.h`

/// evdev `KEY_ENTER`. Re-exposed because `nativeCommitText` converts a
/// literal `"\n"` from the IME into an Enter key event.
pub const EVDEV_KEY_ENTER: u32 = 28;
pub const EVDEV_KEY_ESC: u32 = 1;

/// Translate an Android `KeyEvent.KEYCODE_*` value into the matching
/// Linux evdev keycode. `None` for keys we have no mapping for (media
/// keys, gamepad buttons, Android-specific keys like BACK/HOME/MENU,
/// etc.) — caller logs and drops.
pub fn android_to_evdev(android: i32) -> Option<u32> {
    Some(match android {
        // Editing
        67 => 14,                  // KEYCODE_DEL            -> KEY_BACKSPACE
        112 => 111,                // KEYCODE_FORWARD_DEL    -> KEY_DELETE
        66 => EVDEV_KEY_ENTER,     // KEYCODE_ENTER          -> KEY_ENTER
        61 => 15,                  // KEYCODE_TAB            -> KEY_TAB
        111 => EVDEV_KEY_ESC,      // KEYCODE_ESCAPE         -> KEY_ESC
        62 => 57,                  // KEYCODE_SPACE          -> KEY_SPACE

        // Navigation
        19 => 103,                 // KEYCODE_DPAD_UP        -> KEY_UP
        20 => 108,                 // KEYCODE_DPAD_DOWN      -> KEY_DOWN
        21 => 105,                 // KEYCODE_DPAD_LEFT      -> KEY_LEFT
        22 => 106,                 // KEYCODE_DPAD_RIGHT     -> KEY_RIGHT
        122 => 102,                // KEYCODE_MOVE_HOME      -> KEY_HOME
        123 => 107,                // KEYCODE_MOVE_END       -> KEY_END
        92 => 104,                 // KEYCODE_PAGE_UP        -> KEY_PAGEUP
        93 => 109,                 // KEYCODE_PAGE_DOWN      -> KEY_PAGEDOWN
        124 => 110,                // KEYCODE_INSERT         -> KEY_INSERT

        // Modifiers / locks / system
        59 => 42,                  // KEYCODE_SHIFT_LEFT     -> KEY_LEFTSHIFT
        60 => 54,                  // KEYCODE_SHIFT_RIGHT    -> KEY_RIGHTSHIFT
        113 => 29,                 // KEYCODE_CTRL_LEFT      -> KEY_LEFTCTRL
        114 => 97,                 // KEYCODE_CTRL_RIGHT     -> KEY_RIGHTCTRL
        57 => 56,                  // KEYCODE_ALT_LEFT       -> KEY_LEFTALT
        58 => 100,                 // KEYCODE_ALT_RIGHT      -> KEY_RIGHTALT
        117 => 125,                // KEYCODE_META_LEFT      -> KEY_LEFTMETA
        118 => 126,                // KEYCODE_META_RIGHT     -> KEY_RIGHTMETA
        115 => 58,                 // KEYCODE_CAPS_LOCK      -> KEY_CAPSLOCK
        143 => 69,                 // KEYCODE_NUM_LOCK       -> KEY_NUMLOCK
        116 => 70,                 // KEYCODE_SCROLL_LOCK    -> KEY_SCROLLLOCK
        121 => 119,                // KEYCODE_BREAK          -> KEY_PAUSE
        120 => 99,                 // KEYCODE_SYSRQ          -> KEY_SYSRQ

        // Function keys
        131 => 59,                 // KEYCODE_F1             -> KEY_F1
        132 => 60,                 // KEYCODE_F2             -> KEY_F2
        133 => 61,                 // KEYCODE_F3             -> KEY_F3
        134 => 62,                 // KEYCODE_F4             -> KEY_F4
        135 => 63,                 // KEYCODE_F5             -> KEY_F5
        136 => 64,                 // KEYCODE_F6             -> KEY_F6
        137 => 65,                 // KEYCODE_F7             -> KEY_F7
        138 => 66,                 // KEYCODE_F8             -> KEY_F8
        139 => 67,                 // KEYCODE_F9             -> KEY_F9
        140 => 68,                 // KEYCODE_F10            -> KEY_F10
        141 => 87,                 // KEYCODE_F11            -> KEY_F11
        142 => 88,                 // KEYCODE_F12            -> KEY_F12

        // Letters
        29 => 30,                  // KEYCODE_A              -> KEY_A
        30 => 48,                  // KEYCODE_B              -> KEY_B
        31 => 46,                  // KEYCODE_C              -> KEY_C
        32 => 32,                  // KEYCODE_D              -> KEY_D
        33 => 18,                  // KEYCODE_E              -> KEY_E
        34 => 33,                  // KEYCODE_F              -> KEY_F
        35 => 34,                  // KEYCODE_G              -> KEY_G
        36 => 35,                  // KEYCODE_H              -> KEY_H
        37 => 23,                  // KEYCODE_I              -> KEY_I
        38 => 36,                  // KEYCODE_J              -> KEY_J
        39 => 37,                  // KEYCODE_K              -> KEY_K
        40 => 38,                  // KEYCODE_L              -> KEY_L
        41 => 50,                  // KEYCODE_M              -> KEY_M
        42 => 49,                  // KEYCODE_N              -> KEY_N
        43 => 24,                  // KEYCODE_O              -> KEY_O
        44 => 25,                  // KEYCODE_P              -> KEY_P
        45 => 16,                  // KEYCODE_Q              -> KEY_Q
        46 => 19,                  // KEYCODE_R              -> KEY_R
        47 => 31,                  // KEYCODE_S              -> KEY_S
        48 => 20,                  // KEYCODE_T              -> KEY_T
        49 => 22,                  // KEYCODE_U              -> KEY_U
        50 => 47,                  // KEYCODE_V              -> KEY_V
        51 => 17,                  // KEYCODE_W              -> KEY_W
        52 => 45,                  // KEYCODE_X              -> KEY_X
        53 => 21,                  // KEYCODE_Y              -> KEY_Y
        54 => 44,                  // KEYCODE_Z              -> KEY_Z

        // Top-row digits (Android KEYCODE_0 = 7; evdev KEY_0 = 11, KEY_1..9 = 2..10)
        7 => 11,                   // KEYCODE_0              -> KEY_0
        8 => 2,                    // KEYCODE_1              -> KEY_1
        9 => 3,                    // KEYCODE_2              -> KEY_2
        10 => 4,                   // KEYCODE_3              -> KEY_3
        11 => 5,                   // KEYCODE_4              -> KEY_4
        12 => 6,                   // KEYCODE_5              -> KEY_5
        13 => 7,                   // KEYCODE_6              -> KEY_6
        14 => 8,                   // KEYCODE_7              -> KEY_7
        15 => 9,                   // KEYCODE_8              -> KEY_8
        16 => 10,                  // KEYCODE_9              -> KEY_9

        // Punctuation
        68 => 41,                  // KEYCODE_GRAVE          -> KEY_GRAVE
        69 => 12,                  // KEYCODE_MINUS          -> KEY_MINUS
        70 => 13,                  // KEYCODE_EQUALS         -> KEY_EQUAL
        71 => 26,                  // KEYCODE_LEFT_BRACKET   -> KEY_LEFTBRACE
        72 => 27,                  // KEYCODE_RIGHT_BRACKET  -> KEY_RIGHTBRACE
        73 => 43,                  // KEYCODE_BACKSLASH      -> KEY_BACKSLASH
        74 => 39,                  // KEYCODE_SEMICOLON      -> KEY_SEMICOLON
        75 => 40,                  // KEYCODE_APOSTROPHE     -> KEY_APOSTROPHE
        76 => 53,                  // KEYCODE_SLASH          -> KEY_SLASH
        55 => 51,                  // KEYCODE_COMMA          -> KEY_COMMA
        56 => 52,                  // KEYCODE_PERIOD         -> KEY_DOT

        // Numpad
        144 => 82,                 // KEYCODE_NUMPAD_0       -> KEY_KP0
        145 => 79,                 // KEYCODE_NUMPAD_1       -> KEY_KP1
        146 => 80,                 // KEYCODE_NUMPAD_2       -> KEY_KP2
        147 => 81,                 // KEYCODE_NUMPAD_3       -> KEY_KP3
        148 => 75,                 // KEYCODE_NUMPAD_4       -> KEY_KP4
        149 => 76,                 // KEYCODE_NUMPAD_5       -> KEY_KP5
        150 => 77,                 // KEYCODE_NUMPAD_6       -> KEY_KP6
        151 => 71,                 // KEYCODE_NUMPAD_7       -> KEY_KP7
        152 => 72,                 // KEYCODE_NUMPAD_8       -> KEY_KP8
        153 => 73,                 // KEYCODE_NUMPAD_9       -> KEY_KP9
        154 => 98,                 // KEYCODE_NUMPAD_DIVIDE  -> KEY_KPSLASH
        155 => 55,                 // KEYCODE_NUMPAD_MULTIPLY-> KEY_KPASTERISK
        156 => 74,                 // KEYCODE_NUMPAD_SUBTRACT-> KEY_KPMINUS
        157 => 78,                 // KEYCODE_NUMPAD_ADD     -> KEY_KPPLUS
        158 => 83,                 // KEYCODE_NUMPAD_DOT     -> KEY_KPDOT
        159 => 121,                // KEYCODE_NUMPAD_COMMA   -> KEY_KPCOMMA
        160 => 96,                 // KEYCODE_NUMPAD_ENTER   -> KEY_KPENTER
        161 => 117,                // KEYCODE_NUMPAD_EQUALS  -> KEY_KPEQUAL

        _ => return None,
    })
}
