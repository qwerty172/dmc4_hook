#pragma once

#include "../mod.hpp"
#include <cstdint>

class NeroComboBBuff : public Mod {
public:
    NeroComboBBuff() = default;
    ~NeroComboBBuff() override;

    std::string get_mod_name() override { return "NeroComboBBuff"; }

    // Master toggle
    static bool mod_enabled;

    // ----- Detection: user-configurable Combo B moveIDs -----
    // Nero's Combo B hits each have a distinct moveIDBest value. The user
    // observes the live readout while performing Combo B and fills these in.
    // Up to 4 IDs; 0 = unused slot.
    static int  cb_move_id[4];   // ids for hits 1–4

    // movePart threshold at which we fire the 2nd-hit launcher.
    // User can tweak if their observed move_part values differ.
    static int  launcher_at_part;  // default 1

    // ----- Effect toggles (all on by default) -----
    static bool effect_no_hitstop;   // Effect 1 — no recoil interruption
    static bool effect_launcher;     // Effect 2 — 2nd-hit launcher
    static bool effect_stun;         // Effect 3 — 20× stun multiplier
    static bool effect_exceed;       // Effect 4 — partial Exceed per hit

    // ----- Tunables -----
    static float launcher_strength;      // vertical velocity (default ≈ High Roller)
    static int   stun_multiplier;        // × factor, default 20
    static float exceed_per_hit;         // added to exceedTimer per hit, default 0.5

    // ----- Hook machinery (stun hook at 0x10CA35, chained after KnockbackEdits) -----
    static uintptr_t s_jmp_stun;        // MinHook trampoline → KB_detour → original
    static volatile bool  s_combo_b_active;  // written by on_frame, read by asm hook
    static volatile int   s_stun_int_factor; // int copy of stun_multiplier for asm

    std::optional<std::string> on_initialize() override;
    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;
    void on_gui_frame(int display) override;
    void on_frame(fmilliseconds& dt) override;

private:
    std::unique_ptr<FunctionHook> m_stun_hook;

    // Per-frame state
    uint32_t m_prev_move_id   = 0;
    uint32_t m_prev_move_part = 0;
    int      m_hit_count      = 0;   // hits landed in current Combo B sequence
    bool     m_launcher_fired = false;
    int      m_active_frames  = 0;   // frames since last Combo B move detected

    // Live readout
    uint32_t m_dbg_move_id    = 0;
    uint32_t m_dbg_move_part  = 0;
    bool     m_dbg_is_nero    = false;
    uint8_t  m_dbg_exceed_lvl = 0;
    float    m_dbg_exceed_tmr = 0.0f;
    bool     m_dbg_dt_active  = false;
};
