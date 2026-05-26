#pragma once

#include "../mod.hpp"
#include <cstdint>

class PiercingShots : public Mod {
public:
    PiercingShots() = default;

    static bool mod_enabled;
    static bool trigger_on_dt;
    static bool trigger_on_charge;

    std::string get_mod_name() override { return "PiercingShots"; };

    std::optional<std::string> on_initialize() override;
    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;
    void on_gui_frame(int display) override;
    void on_frame(fmilliseconds& dt) override;

private:
    // Try to restore the originals on whatever `inner` we last modified.
    // Returns true if fully restored (or nothing to restore); false if a
    // restore is still pending (we never managed to get the pointer back).
    bool try_restore();

    // The exact InnerMotionPtr instance we wrote into. We restore to *this*
    // pointer on disable, not whatever the current player happens to expose
    // (e.g. after a character swap to Nero).
    uintptr_t m_tracked_inner = 0;

    // Original passThroughEnemiesIdk values, captured the first time we
    // overwrite each entry. Indexed 0..7: [4 ground][4 air].
    uint32_t m_original[8] = {0};
    bool     m_captured    = false;
};
