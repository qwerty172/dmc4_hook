#pragma once

#include "../mod.hpp"
#include <cstdint>

class NeroDiagonalAirHike : public Mod {
public:
    NeroDiagonalAirHike() = default;
    ~NeroDiagonalAirHike() override = default;

    std::string get_mod_name() override { return "NeroDiagonalAirHike"; }

    static bool mod_enabled;

    // Tunables
    static float total_speed;     // total launch speed, default 14.0
    static float angle_deg;       // upward angle from horizontal, default 45.0
    static bool  use_big_jump;    // write triggerBigJump for animation, default true
    static int   anim_id_override;// 0 = don't override animID

    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;
    void on_gui_frame(int display) override;
    void on_frame(fmilliseconds& dt) override;

private:
    uint8_t m_prev_hike_count = 0;
    int     m_apply_frames    = 0;  // countdown — write velocity for N frames

    // Live readout
    bool     m_dbg_grounded   = true;
    uint8_t  m_dbg_hike_count = 0;
    uint16_t m_dbg_anim_id    = 0;
    uint32_t m_dbg_move_id    = 0;
};
