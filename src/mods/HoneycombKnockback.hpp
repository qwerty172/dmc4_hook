#pragma once

#include "../mod.hpp"
#include <cstdint>

class HoneycombKnockback : public Mod {
public:
    HoneycombKnockback() = default;

    static bool  mod_enabled;

    // ----- Detection: hook on the Twosome Time / Honeycomb handler -----
    // Absolute address provided by the user (function in DevilMayCry4_DX9.exe
    // that runs when Twosome Time / Honeycomb Fire is processed).
    static constexpr uintptr_t HONEYCOMB_HOOK_ADDR = 0x00C40DBC;

    // Frames remaining during which we consider Honeycomb "active" after the
    // last time the hooked function ran. Bridges gaps between rapid-fire calls.
    static int   hook_active_window_frames;

    // Hook-side "last hit" frame counter; decremented every on_frame. > 0 means
    // active. Made public so the asm detour can write to it directly.
    static volatile int s_hook_active_counter;

    // True once install_hook_absolute succeeded.
    static bool  s_hook_installed;

    // ----- Fallback detection: moveIDBest comparison (in case the hook fails
    // or the user prefers it). 0 = disabled. -----
    static int   honeycomb_move_id;

    // ----- WASD source: bits inside uPlayer.inputHold[] (0x140C..0x140F) -----
    // Default guesses for XInput-like d-pad mapping. The user discovers the
    // real values in-game via the live inputHold[] readout.
    static int   input_byte_index;   // 0..3 (which byte of inputHold[])
    static int   bit_forward;        // bitmask, e.g. 0x01
    static int   bit_back;           // bitmask, e.g. 0x02
    static int   bit_left;           // bitmask, e.g. 0x04
    static int   bit_right;          // bitmask, e.g. 0x08

    // ----- Behavior tunables -----
    // Orbit mode (DT only by default): A/D tangentially around player→enemy
    // line, W/S vertical (via camera-forward projection). Radial component
    // (toward/away from player) is dropped.
    static float vertical_strength_dt;
    static float horizontal_strength_dt;

    // Fallback (no DT, no camera-relative): always pushes straight up.
    static float vertical_strength_no_dt;

    // Range filter for non-lockon mode.
    static float max_range;
    static bool  affect_only_locked_on;

    // ----- New: camera-relative mode -----
    // When enabled, WASD push the enemy in pure camera-relative XZ (W = camera
    // forward, S = back, A = left, D = right). NO radial filtering — direction
    // is exactly what the player presses, around the camera. Works regardless
    // of DT (the "no DT = always up" fallback is overridden).
    static bool  camera_relative_mode;
    static float camera_relative_strength;     // XZ push strength
    static float camera_relative_vertical;     // optional Y bias when any WASD
                                               // is held (0 = pure horizontal)

    std::string get_mod_name() override { return "HoneycombKnockback"; };

    std::optional<std::string> on_initialize() override;
    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;
    void on_gui_frame(int display) override;
    void on_frame(fmilliseconds& dt) override;

    ~HoneycombKnockback() override;

private:
    // Debug readout state.
    uint32_t m_last_move_id      = 0;
    uint8_t  m_last_input_hold[4] = {0, 0, 0, 0};
    bool     m_last_is_dante     = false;

    std::unique_ptr<FunctionHook> m_hook;
    static uintptr_t              s_hook_trampoline;
};
