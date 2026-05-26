#include "PiercingShots.hpp"
#include "../sdk/Devil4.hpp"

bool PiercingShots::mod_enabled       = false;
bool PiercingShots::trigger_on_dt     = true;
bool PiercingShots::trigger_on_charge = true;

// Ebony & Ivory shot motion IDs (from DANTE_ATCK_ID in ReClass_Internal.hpp).
// Ground + air, regular + charge Lv1..Lv3. Always touches both ground and air.
// Deliberately excludes Twosome Time / Honeycomb / Rainstorm (Gunslinger moves)
// and Coyote Ace / Pandora.
static constexpr uint32_t EI_IDS[8] = {
    0x3C, 0x3D, 0x3E, 0x3F,   // ground: regular, charge Lv1, Lv2, Lv3
    0x40, 0x41, 0x42, 0x43,   // air:    regular, charge Lv1, Lv2, Lv3
};

// Charge level byte on the player (0..3), reused from PandoraCharge.cpp.
static constexpr uintptr_t PLAYER_CHARGE_LV_OFFSET = 0x14DA0;

static InnerMotionPtr* current_inner() {
    auto* player = devil4_sdk::get_local_player();
    if (player == nullptr) {
        return nullptr;
    }
    MotionPtr* mp = player->motionPtr1;
    if (mp == nullptr) {
        return nullptr;
    }
    return mp->innerMotionPtr1;
}

bool PiercingShots::try_restore() {
    if (!m_captured) {
        m_tracked_inner = 0;
        return true;
    }
    if (m_tracked_inner == 0) {
        m_captured = false;
        return true;
    }
    // Safe-restore guard: only write back if the currently-live local player
    // still exposes the exact InnerMotionPtr we captured against. Otherwise
    // drop tracked state without writing (avoid touching freed memory).
    InnerMotionPtr* live_inner = current_inner();
    if (live_inner == nullptr ||
        reinterpret_cast<uintptr_t>(live_inner) != m_tracked_inner) {
        m_captured      = false;
        m_tracked_inner = 0;
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        const uint32_t id = EI_IDS[i];
        if (id >= 100) continue;
        live_inner->motionData[id].passThroughEnemiesIdk = m_original[i];
    }
    m_captured      = false;
    m_tracked_inner = 0;
    return true;
}

void PiercingShots::on_frame(fmilliseconds& dt) {
    if (!mod_enabled) {
        try_restore();
        return;
    }

    auto* player = devil4_sdk::get_local_player();
    if (player == nullptr) {
        return;
    }

    // Dante only (controllerID: 0 = Dante, 1 = Nero).
    if (player->controllerID != 0) {
        try_restore();
        return;
    }

    InnerMotionPtr* inner = current_inner();
    if (inner == nullptr) {
        return;
    }

    // Character swap / table reload: restore old, then track new.
    if (m_captured && m_tracked_inner != reinterpret_cast<uintptr_t>(inner)) {
        try_restore();
    }
    m_tracked_inner = reinterpret_cast<uintptr_t>(inner);

    const bool dt_active = trigger_on_dt && (player->DT != 0.0f);
    const uint8_t charge_lv =
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(player) +
                                    PLAYER_CHARGE_LV_OFFSET);
    // Any non-zero charge level counts (Lv1, Lv2 or Lv3).
    const bool charged   = trigger_on_charge && (charge_lv > 0);
    const uint32_t value = (dt_active || charged) ? 1u : 0u;

    if (!m_captured) {
        for (size_t i = 0; i < 8; ++i) {
            const uint32_t id = EI_IDS[i];
            m_original[i] = (id < 100)
                ? inner->motionData[id].passThroughEnemiesIdk
                : 0u;
        }
        m_captured = true;
    }
    for (size_t i = 0; i < 8; ++i) {
        const uint32_t id = EI_IDS[i];
        if (id >= 100) continue;
        inner->motionData[id].passThroughEnemiesIdk = value;
    }
}

std::optional<std::string> PiercingShots::on_initialize() {
    return Mod::on_initialize();
}

void PiercingShots::on_gui_frame(int display) {
    if (ImGui::Checkbox(_("Piercing E&I Shots"), &mod_enabled)) {
        if (!mod_enabled) {
            try_restore();
        }
    }
    ImGui::SameLine();
    help_marker(_("Make Ebony & Ivory shots (ground and air) pass through "
                  "enemies when Devil Trigger is active or the shot is charged "
                  "(any level). Dante only. Does not affect Gunslinger gun "
                  "moves (Honeycomb / Rainstorm / Twosome Time / Gun Stinger)."));

    if (mod_enabled) {
        ImGui::Indent(lineIndent);
        ImGui::Checkbox(_("Trigger on Devil Trigger"), &trigger_on_dt);
        ImGui::Checkbox(_("Trigger on any Charge"),    &trigger_on_charge);
        ImGui::Unindent(lineIndent);
    }
}

void PiercingShots::on_config_load(const utility::Config& cfg) {
    mod_enabled       = cfg.get<bool>("piercing_shots_enabled").value_or(false);
    trigger_on_dt     = cfg.get<bool>("piercing_shots_on_dt").value_or(true);
    trigger_on_charge = cfg.get<bool>("piercing_shots_on_charge").value_or(true);
}

void PiercingShots::on_config_save(utility::Config& cfg) {
    cfg.set<bool>("piercing_shots_enabled",   mod_enabled);
    cfg.set<bool>("piercing_shots_on_dt",     trigger_on_dt);
    cfg.set<bool>("piercing_shots_on_charge", trigger_on_charge);
}
