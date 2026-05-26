#include "NeroDiagonalAirHike.hpp"
#include "../sdk/Devil4.hpp"
#include <cmath>

bool  NeroDiagonalAirHike::mod_enabled      = false;
float NeroDiagonalAirHike::total_speed      = 14.0f;
float NeroDiagonalAirHike::angle_deg        = 45.0f;
bool  NeroDiagonalAirHike::use_big_jump     = true;
int   NeroDiagonalAirHike::anim_id_override = 0;

static bool get_cam_fwd(float& fx, float& fz) {
    auto* cam = devil4_sdk::get_player_camera();
    if (cam == nullptr) return false;
    fx = cam->lookat.x - cam->pos.x;
    fz = cam->lookat.z - cam->pos.z;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len < 0.001f) return false;
    fx /= len;
    fz /= len;
    return true;
}

void NeroDiagonalAirHike::on_frame(fmilliseconds& dt) {
    uPlayer* player = devil4_sdk::get_local_player();
    if (player == nullptr) return;

    bool grounded = (player->grounded > 0) || player->grounded2;

    m_dbg_grounded   = grounded;
    m_dbg_hike_count = player->airHikeCount;
    m_dbg_anim_id    = player->animID;
    m_dbg_move_id    = player->moveIDBest;

    if (!mod_enabled || player->controllerID != 1) {
        m_prev_hike_count = player->airHikeCount;
        m_apply_frames    = 0;
        return;
    }

    // Reset counters on landing.
    if (grounded) {
        m_prev_hike_count = 0;
        m_apply_frames    = 0;
        return;
    }

    // Rising-edge on airHikeCount while airborne = Air Hike just used.
    const uint8_t cur = player->airHikeCount;
    if (cur > m_prev_hike_count && m_prev_hike_count == 0) {
        m_apply_frames = 3; // override velocity for 3 frames
    }
    m_prev_hike_count = cur;

    if (m_apply_frames <= 0) return;
    --m_apply_frames;

    // Compute 45°-diagonal velocity in camera-forward direction.
    const float rad   = angle_deg * (3.14159265f / 180.0f);
    const float horiz = total_speed * std::cosf(rad);
    const float vert  = total_speed * std::sinf(rad);

    float fx = 0.0f, fz = 0.0f;
    const bool has_cam = get_cam_fwd(fx, fz);

    player->m_d_velocity.x  = has_cam ? fx * horiz : 0.0f;
    player->m_d_velocity.y  = vert;
    player->m_d_velocity.z  = has_cam ? fz * horiz : 0.0f;
    player->m_d_vel_magnitude = total_speed;

    if (use_big_jump)
        player->triggerBigJump = true;

    if (anim_id_override > 0 && anim_id_override <= 0xFFFF)
        player->animID = static_cast<uint16_t>(anim_id_override);
}

void NeroDiagonalAirHike::on_gui_frame(int display) {
    if (display != 1) return;

    ImGui::Checkbox(_("Nero Diagonal Air Hike"), &mod_enabled);
    ImGui::SameLine();
    help_marker(_(
        "Replaces Nero's vertical Air Hike with a diagonal dash in the "
        "direction the camera is facing.\n\n"
        "On each Air Hike, velocity is redirected at the configured angle "
        "from horizontal — 0 = pure horizontal dash, 90 = straight up. "
        "Default 45° gives an equal forward/upward diagonal."));

    if (!mod_enabled) return;

    ImGui::Indent(lineIndent);

    ImGui::Text(_("animID: 0x%04X   move: 0x%04X   grounded: %s   hikes: %d"),
                static_cast<uint32_t>(m_dbg_anim_id),
                m_dbg_move_id,
                m_dbg_grounded ? _("yes") : _("NO"),
                static_cast<int>(m_dbg_hike_count));
    ImGui::SameLine();
    help_marker(_("Live readout. Note animID values during Air Hike or "
                  "other aerial moves to find a good flip animation."));

    ImGui::Separator();

    ImGui::SliderFloat(_("Total speed"), &total_speed, 0.0f, 40.0f, "%.1f");
    ImGui::SameLine();
    help_marker(_("Combined launch speed. Default 14 is roughly 2× a "
                  "standard Air Hike."));

    ImGui::SliderFloat(_("Angle (deg)"), &angle_deg, 0.0f, 90.0f, "%.1f°");
    ImGui::SameLine();
    help_marker(_("Upward angle from horizontal. 0 = pure forward dash, "
                  "45 = equal forward+up diagonal, 90 = straight up."));

    ImGui::Separator();

    ImGui::Checkbox(_("triggerBigJump (flip anim attempt)"), &use_big_jump);
    ImGui::SameLine();
    help_marker(_("Sets the triggerBigJump flag on the dash frame. "
                  "May play a forward-jump animation — test in-game."));

    ImGui::PushItemWidth(100.0f);
    ImGui::InputInt(_("animID override (0 = off)"), &anim_id_override);
    if (anim_id_override < 0)      anim_id_override = 0;
    if (anim_id_override > 0xFFFF) anim_id_override = 0xFFFF;
    ImGui::PopItemWidth();
    ImGui::SameLine();
    help_marker(_("Force a specific animation on the dash frame. "
                  "Find the right ID by watching the 'animID' readout "
                  "above while Nero performs aerial moves. 0 = disabled."));

    ImGui::Unindent(lineIndent);
}

void NeroDiagonalAirHike::on_config_load(const utility::Config& cfg) {
    mod_enabled      = cfg.get<bool> ("nero_diag_hike_on").value_or(false);
    total_speed      = cfg.get<float>("nero_diag_hike_speed").value_or(14.0f);
    angle_deg        = cfg.get<float>("nero_diag_hike_angle").value_or(45.0f);
    use_big_jump     = cfg.get<bool> ("nero_diag_hike_bigjump").value_or(true);
    anim_id_override = cfg.get<int>  ("nero_diag_hike_animid").value_or(0);
    if (angle_deg  <  0.0f) angle_deg  =  0.0f;
    if (angle_deg  > 90.0f) angle_deg  = 90.0f;
    if (total_speed < 0.0f) total_speed = 0.0f;
}

void NeroDiagonalAirHike::on_config_save(utility::Config& cfg) {
    cfg.set<bool> ("nero_diag_hike_on",      mod_enabled);
    cfg.set<float>("nero_diag_hike_speed",   total_speed);
    cfg.set<float>("nero_diag_hike_angle",   angle_deg);
    cfg.set<bool> ("nero_diag_hike_bigjump", use_big_jump);
    cfg.set<int>  ("nero_diag_hike_animid",  anim_id_override);
}
