#include "HoneycombKnockback.hpp"
#include "../sdk/Devil4.hpp"
#include <cmath>

bool  HoneycombKnockback::mod_enabled             = false;

// Hook-detection state.
int   HoneycombKnockback::hook_active_window_frames = 30;   // ~0.5s @ 60fps
volatile int HoneycombKnockback::s_hook_active_counter = 0;
bool  HoneycombKnockback::s_hook_installed        = false;
uintptr_t HoneycombKnockback::s_hook_trampoline   = 0;

// Fallback: moveIDBest comparison (0 = disabled).
int   HoneycombKnockback::honeycomb_move_id       = 0;

// XInput-style d-pad guesses. User overrides via UI after observing the
// live inputHold[] readout while pressing W/A/S/D.
int   HoneycombKnockback::input_byte_index        = 0;
int   HoneycombKnockback::bit_forward             = 0x01;
int   HoneycombKnockback::bit_back                = 0x02;
int   HoneycombKnockback::bit_left                = 0x04;
int   HoneycombKnockback::bit_right               = 0x08;

float HoneycombKnockback::vertical_strength_dt    = 1.0f;
float HoneycombKnockback::horizontal_strength_dt  = 0.33f;
float HoneycombKnockback::vertical_strength_no_dt = 1.0f;
float HoneycombKnockback::max_range               = 30.0f;
bool  HoneycombKnockback::affect_only_locked_on   = true;

bool  HoneycombKnockback::camera_relative_mode    = false;
float HoneycombKnockback::camera_relative_strength = 0.33f;
float HoneycombKnockback::camera_relative_vertical = 0.0f;

// ---------------------------------------------------------------------------

static inline uint8_t safe_byte(const uint8_t* bytes, int idx) {
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return bytes[idx];
}

struct DirInput {
    bool fwd, back, left, right;
    bool any() const { return fwd || back || left || right; }
    // Net signed values along the two stick axes.
    float ud() const { return (fwd  ? 1.0f : 0.0f) - (back  ? 1.0f : 0.0f); }
    float lr() const { return (right? 1.0f : 0.0f) - (left  ? 1.0f : 0.0f); }
};

static DirInput read_dir_input(const uint8_t* hold) {
    const uint8_t b = safe_byte(hold, HoneycombKnockback::input_byte_index);
    DirInput d;
    d.fwd   = (b & static_cast<uint8_t>(HoneycombKnockback::bit_forward)) != 0;
    d.back  = (b & static_cast<uint8_t>(HoneycombKnockback::bit_back))    != 0;
    d.left  = (b & static_cast<uint8_t>(HoneycombKnockback::bit_left))    != 0;
    d.right = (b & static_cast<uint8_t>(HoneycombKnockback::bit_right))   != 0;
    return d;
}

static void apply_velocity(uEnemy* enemy, float vx, float vy, float vz) {
    if (enemy == nullptr) return;
    enemy->velocity.x = vx;
    enemy->velocity.y = vy;
    enemy->velocity.z = vz;
}

// Get camera forward / right unit vectors in world XZ.
static bool get_camera_axes_xz(float& fx, float& fz, float& rx, float& rz) {
    auto* cam = devil4_sdk::get_player_camera();
    if (cam == nullptr) return false;
    fx = cam->lookat.x - cam->pos.x;
    fz = cam->lookat.z - cam->pos.z;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len < 0.001f) return false;
    fx /= len;
    fz /= len;
    // Right = forward rotated -90° around world up (Y): (fz, -fx).
    rx =  fz;
    rz = -fx;
    return true;
}

// ---------------------------------------------------------------------------
// Mode A: orbit around the enemy on a sphere centred on the player.
//   - A/D tangential to (player→enemy) on XZ
//   - W/S vertical (via projection onto camera-forward — already meaningful
//     because we use raw WASD, not the locked stick)
// Mode B: pure camera-relative XZ push, no radial filtering, works without DT.
// Fallback (no DT, mode A): always straight up.
// ---------------------------------------------------------------------------
static void compute_and_apply(uEnemy* enemy, uPlayer* player,
                              bool dt_active, const DirInput& dir) {
    if (enemy == nullptr || player == nullptr) return;

    // ----- Mode B: camera-relative -----
    if (HoneycombKnockback::camera_relative_mode) {
        if (!dir.any()) {
            // No WASD held — don't touch the enemy's velocity (let bullets
            // keep them in the air on their own; Y bias only applies when
            // the player is actively pushing direction).
            return;
        }
        float fx, fz, rx, rz;
        if (!get_camera_axes_xz(fx, fz, rx, rz)) {
            return;
        }
        const float ud = dir.ud();
        const float lr = dir.lr();
        const float s  = HoneycombKnockback::camera_relative_strength;
        const float vx = (fx * ud + rx * lr) * s;
        const float vz = (fz * ud + rz * lr) * s;
        const float vy = HoneycombKnockback::camera_relative_vertical;
        apply_velocity(enemy, vx, vy, vz);
        return;
    }

    // ----- Mode A: orbit (the original spec) -----
    if (!dt_active) {
        // No DT, no camera-relative: always lift straight up.
        apply_velocity(enemy, 0.0f,
                       HoneycombKnockback::vertical_strength_no_dt, 0.0f);
        return;
    }

    if (!dir.any()) {
        // DT on but no WASD input — keep them gently floating.
        apply_velocity(enemy, 0.0f,
                       HoneycombKnockback::vertical_strength_no_dt, 0.0f);
        return;
    }

    // Build the WASD world vector via camera axes.
    float fx, fz, rx, rz;
    if (!get_camera_axes_xz(fx, fz, rx, rz)) {
        apply_velocity(enemy, 0.0f,
                       HoneycombKnockback::vertical_strength_no_dt, 0.0f);
        return;
    }
    const float ud = dir.ud();  // W=+1, S=-1
    const float lr = dir.lr();  // D=+1, A=-1
    const float wx = fx * ud + rx * lr;  // WASD direction in world XZ
    const float wz = fz * ud + rz * lr;

    // Player→enemy horizontal direction.
    const float ex = enemy->position.x - player->m_pos.x;
    const float ez = enemy->position.z - player->m_pos.z;
    const float elen = std::sqrt(ex * ex + ez * ez);
    float px, pz;  // tangent (CW perpendicular to player→enemy)
    if (elen < 0.001f) {
        // Degenerate: just use the WASD vector as tangent.
        px = wx;
        pz = wz;
    } else {
        const float dx = ex / elen;
        const float dz = ez / elen;
        px = -dz;
        pz =  dx;
    }

    // A/D (lr) → tangential (perpendicular to player→enemy), W/S (ud) → vertical.
    const float side = lr;                       // pure A/D
    const float vert = ud;                       // pure W/S
    const float vx = px * side * HoneycombKnockback::horizontal_strength_dt;
    const float vz = pz * side * HoneycombKnockback::horizontal_strength_dt;
    const float vy = vert       * HoneycombKnockback::vertical_strength_dt;
    apply_velocity(enemy, vx, vy, vz);
}

// ---------------------------------------------------------------------------
// Hook detour at HONEYCOMB_HOOK_ADDR (0x00C40DBC, Twosome Time / Honeycomb
// handler). We don't know the function's signature or its prologue, so we use
// a naked detour that preserves all registers, refreshes the activity counter,
// then jumps through MinHook's auto-generated trampoline.
// ---------------------------------------------------------------------------
naked void honeycomb_detour() {
    _asm {
        pushad
        pushfd
        mov eax, [HoneycombKnockback::hook_active_window_frames]
        mov [HoneycombKnockback::s_hook_active_counter], eax
        popfd
        popad
        jmp [HoneycombKnockback::s_hook_trampoline]
    }
}

// ---------------------------------------------------------------------------

void HoneycombKnockback::on_frame(fmilliseconds& dt) {
    auto* player = devil4_sdk::get_local_player();
    if (player == nullptr) return;

    // Always update the live debug readout, even if the mod is disabled, so
    // the user can discover the right move ID / input bits without enabling.
    m_last_move_id      = player->moveIDBest;
    m_last_is_dante     = (player->controllerID == 0);
    m_last_input_hold[0] = player->inputHold[0];
    m_last_input_hold[1] = player->inputHold[1];
    m_last_input_hold[2] = player->inputHold[2];
    m_last_input_hold[3] = player->inputHold[3];

    // Decrement the hook-activity counter every frame (regardless of
    // mod_enabled — the hook stays installed for the lifetime of the mod).
    if (s_hook_active_counter > 0) {
        s_hook_active_counter = s_hook_active_counter - 1;
    }

    if (!mod_enabled) return;
    if (!m_last_is_dante) return;

    // Active = hook saw the handler recently OR moveID fallback matches.
    const bool hook_active = (s_hook_installed && s_hook_active_counter > 0);
    const bool move_id_active =
        (honeycomb_move_id != 0 &&
         static_cast<uint32_t>(honeycomb_move_id) == m_last_move_id);
    if (!hook_active && !move_id_active) {
        return;
    }

    const bool dt_active = player->dtActive;
    const DirInput dir   = read_dir_input(player->inputHold);

    if (affect_only_locked_on) {
        uEnemy* target = player->lockOnTargetPtr3;
        if (target == nullptr) target = player->lockOnTargetPtr1;
        if (target == nullptr) target = player->lockOnTargetPtr2;
        if (target == nullptr) target = player->lockOnTargetPtr4;
        if (target != nullptr) {
            compute_and_apply(target, player, dt_active, dir);
        }
    } else {
        const float r2 = max_range * max_range;
        uEnemy* enemy = devil4_sdk::get_uEnemies();
        int safety = 64;
        while (enemy != nullptr && safety-- > 0) {
            const float ex = enemy->position.x - player->m_pos.x;
            const float ey = enemy->position.y - player->m_pos.y;
            const float ez = enemy->position.z - player->m_pos.z;
            const float d2 = ex * ex + ey * ey + ez * ez;
            if (d2 <= r2) {
                compute_and_apply(enemy, player, dt_active, dir);
            }
            enemy = enemy->nextEnemy;
        }
    }
}

std::optional<std::string> HoneycombKnockback::on_initialize() {
    // Install the hook at the absolute address of the Twosome Time / Honeycomb
    // handler. Failure is not fatal — the moveID fallback still works.
    if (install_hook_absolute(HONEYCOMB_HOOK_ADDR, m_hook,
                              reinterpret_cast<void*>(&honeycomb_detour),
                              &s_hook_trampoline, 0)) {
        s_hook_installed = true;
    } else {
        s_hook_installed = false;
        spdlog::error("HoneycombKnockback: failed to hook 0x{:X}, "
                      "falling back to moveID detection.",
                      static_cast<uintptr_t>(HONEYCOMB_HOOK_ADDR));
    }
    return Mod::on_initialize();
}

HoneycombKnockback::~HoneycombKnockback() {
    // Tear down hook explicitly so the static trampoline can never be jumped
    // through after the underlying memory is freed.
    m_hook.reset();
    s_hook_installed     = false;
    s_hook_trampoline    = 0;
    s_hook_active_counter = 0;
}

void HoneycombKnockback::on_gui_frame(int display) {
    ImGui::Checkbox(_("Honeycomb Knockback"), &mod_enabled);
    ImGui::SameLine();
    help_marker(_("During Gunslinger Honeycomb Fire (Gunslinger Lv3, E&I, "
                  "hold shoot two-handed), use the movement keys (WASD) to "
                  "push the locked-on enemy around. Dante only.\n\n"
                  "Two modes (see below):\n"
                  "  - Orbit (default): A/D orbits enemy around player, W/S "
                  "lifts up/down; no toward/away push; without DT enemy is "
                  "always pushed straight up.\n"
                  "  - Camera-relative: pure camera-space WASD push; works "
                  "with or without DT.\n\n"
                  "REQUIRED on first use: set the Honeycomb Move ID and the "
                  "WASD input bits (see live readouts)."));

    ImGui::Indent(lineIndent);

    // ----- Live debug readouts (always visible, even when mod is off) -----
    ImGui::Text(_("Live move ID: 0x%X  (%s)"),
                m_last_move_id,
                m_last_is_dante ? _("Dante") : _("not Dante"));
    ImGui::SameLine();
    help_marker(_("Perform Honeycomb Fire and note the value shown, then "
                  "enter it in 'Honeycomb Move ID' below."));

    ImGui::Text(_("Live inputHold[]: %02X %02X %02X %02X"),
                m_last_input_hold[0], m_last_input_hold[1],
                m_last_input_hold[2], m_last_input_hold[3]);
    ImGui::SameLine();
    help_marker(_("Press W (or A/S/D) and watch which byte/bit toggles. "
                  "Enter the byte index and the four bit masks below."));

    ImGui::Text(_("Honeycomb hook: %s   active counter: %d"),
                s_hook_installed ? _("installed") : _("FAILED"),
                static_cast<int>(s_hook_active_counter));
    ImGui::SameLine();
    help_marker(_("Detection via hook on the Twosome Time / Honeycomb "
                  "handler at 0x00C40DBC. The counter ticks down each frame; "
                  "as long as it's > 0, the mod treats Honeycomb as active. "
                  "If the hook failed to install, the moveID fallback below "
                  "is used instead."));

    ImGui::Separator();

    // ----- Hook activity window -----
    ImGui::InputInt(_("Hook active window (frames)"),
                    &hook_active_window_frames);
    if (hook_active_window_frames < 1)   hook_active_window_frames = 1;
    if (hook_active_window_frames > 600) hook_active_window_frames = 600;
    ImGui::SameLine();
    help_marker(_("How many frames after the last hook hit the mod still "
                  "considers Honeycomb active. Bridges gaps between rapid-fire "
                  "shots. Default 30 (~0.5s at 60fps)."));

    // ----- Honeycomb move ID fallback -----
    ImGui::InputInt(_("Honeycomb Move ID (fallback)"), &honeycomb_move_id);
    ImGui::SameLine();
    help_marker(_("Backup detection: compare uPlayer.moveIDBest to this "
                  "value. Used in addition to the hook. Leave 0 to disable."));

    // ----- WASD bit configuration -----
    ImGui::InputInt(_("Input byte index (0-3)"), &input_byte_index);
    if (input_byte_index < 0) input_byte_index = 0;
    if (input_byte_index > 3) input_byte_index = 3;

    ImGui::InputInt(_("Bit: forward (W)"), &bit_forward);
    ImGui::InputInt(_("Bit: back (S)"),    &bit_back);
    ImGui::InputInt(_("Bit: left (A)"),    &bit_left);
    ImGui::InputInt(_("Bit: right (D)"),   &bit_right);

    ImGui::Separator();

    if (!mod_enabled) {
        ImGui::TextDisabled(_("(enable the mod above to use these tunables)"));
        ImGui::Unindent(lineIndent);
        return;
    }

    ImGui::Checkbox(_("Affect only lock-on target"), &affect_only_locked_on);
    if (!affect_only_locked_on) {
        ImGui::SliderFloat(_("Range"), &max_range, 1.0f, 100.0f, "%.1f");
    }

    ImGui::Separator();

    ImGui::Checkbox(_("Camera-relative mode (works without DT)"),
                    &camera_relative_mode);
    ImGui::SameLine();
    help_marker(_("Pushes the enemy purely along camera axes (W=forward, "
                  "S=back, A=left, D=right). No 'orbit around player' math, "
                  "no DT requirement."));

    if (camera_relative_mode) {
        ImGui::SliderFloat(_("Camera-rel. XZ strength"),
                           &camera_relative_strength, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat(_("Camera-rel. Y bias"),
                           &camera_relative_vertical, -2.0f, 2.0f, "%.2f");
    } else {
        ImGui::SliderFloat(_("Vertical strength (DT)"),
                           &vertical_strength_dt, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat(_("Sideways strength (DT)"),
                           &horizontal_strength_dt, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat(_("Vertical strength (no DT)"),
                           &vertical_strength_no_dt, 0.0f, 5.0f, "%.2f");
    }

    ImGui::Unindent(lineIndent);
}

void HoneycombKnockback::on_config_load(const utility::Config& cfg) {
    mod_enabled              = cfg.get<bool>("honeycomb_kb_enabled").value_or(false);
    hook_active_window_frames= cfg.get<int>("honeycomb_kb_hook_window").value_or(30);
    if (hook_active_window_frames < 1)   hook_active_window_frames = 1;
    if (hook_active_window_frames > 600) hook_active_window_frames = 600;
    honeycomb_move_id        = cfg.get<int>("honeycomb_kb_move_id").value_or(0);
    input_byte_index         = cfg.get<int>("honeycomb_kb_byte_idx").value_or(0);
    bit_forward              = cfg.get<int>("honeycomb_kb_bit_fwd").value_or(0x01);
    bit_back                 = cfg.get<int>("honeycomb_kb_bit_back").value_or(0x02);
    bit_left                 = cfg.get<int>("honeycomb_kb_bit_left").value_or(0x04);
    bit_right                = cfg.get<int>("honeycomb_kb_bit_right").value_or(0x08);
    vertical_strength_dt     = cfg.get<float>("honeycomb_kb_vert_dt").value_or(1.0f);
    horizontal_strength_dt   = cfg.get<float>("honeycomb_kb_horiz_dt").value_or(0.33f);
    vertical_strength_no_dt  = cfg.get<float>("honeycomb_kb_vert_no_dt").value_or(1.0f);
    max_range                = cfg.get<float>("honeycomb_kb_range").value_or(30.0f);
    affect_only_locked_on    = cfg.get<bool>("honeycomb_kb_lockon").value_or(true);
    camera_relative_mode     = cfg.get<bool>("honeycomb_kb_camrel").value_or(false);
    camera_relative_strength = cfg.get<float>("honeycomb_kb_camrel_xz").value_or(0.33f);
    camera_relative_vertical = cfg.get<float>("honeycomb_kb_camrel_y").value_or(0.0f);
}

void HoneycombKnockback::on_config_save(utility::Config& cfg) {
    cfg.set<bool> ("honeycomb_kb_enabled",   mod_enabled);
    cfg.set<int>  ("honeycomb_kb_hook_window", hook_active_window_frames);
    cfg.set<int>  ("honeycomb_kb_move_id",   honeycomb_move_id);
    cfg.set<int>  ("honeycomb_kb_byte_idx",  input_byte_index);
    cfg.set<int>  ("honeycomb_kb_bit_fwd",   bit_forward);
    cfg.set<int>  ("honeycomb_kb_bit_back",  bit_back);
    cfg.set<int>  ("honeycomb_kb_bit_left",  bit_left);
    cfg.set<int>  ("honeycomb_kb_bit_right", bit_right);
    cfg.set<float>("honeycomb_kb_vert_dt",   vertical_strength_dt);
    cfg.set<float>("honeycomb_kb_horiz_dt",  horizontal_strength_dt);
    cfg.set<float>("honeycomb_kb_vert_no_dt",vertical_strength_no_dt);
    cfg.set<float>("honeycomb_kb_range",     max_range);
    cfg.set<bool> ("honeycomb_kb_lockon",    affect_only_locked_on);
    cfg.set<bool> ("honeycomb_kb_camrel",    camera_relative_mode);
    cfg.set<float>("honeycomb_kb_camrel_xz", camera_relative_strength);
    cfg.set<float>("honeycomb_kb_camrel_y",  camera_relative_vertical);
}
