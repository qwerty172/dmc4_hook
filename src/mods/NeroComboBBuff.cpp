#include "NeroComboBBuff.hpp"
#include "../sdk/Devil4.hpp"

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
bool  NeroComboBBuff::mod_enabled        = false;

int   NeroComboBBuff::cb_move_id[4]      = {0, 0, 0, 0};
int   NeroComboBBuff::launcher_at_part   = 1;

bool  NeroComboBBuff::effect_no_hitstop  = true;
bool  NeroComboBBuff::effect_launcher    = true;
bool  NeroComboBBuff::effect_stun        = true;
bool  NeroComboBBuff::effect_exceed      = true;

float NeroComboBBuff::launcher_strength  = 8.0f;
int   NeroComboBBuff::stun_multiplier    = 20;
float NeroComboBBuff::exceed_per_hit     = 0.5f;

uintptr_t            NeroComboBBuff::s_jmp_stun        = 0;
volatile bool        NeroComboBBuff::s_combo_b_active  = false;
volatile int         NeroComboBBuff::s_stun_int_factor = 20;

// ---------------------------------------------------------------------------
// Stun hook — chained after KnockbackEdits which also hooks 0x10CA35.
// edx = pointer to attack-source data; kAttackStatus_v3 lives at [edx+0xA4].
// mHitStopTimer is at kAttackStatus_v3+0x28, so at [edx+0xA4+0x28].
// ---------------------------------------------------------------------------
static __declspec(naked) void nero_stun_detour() {
    _asm {
        cmp byte ptr [NeroComboBBuff::s_combo_b_active], 1
        jne dochain
        cmp byte ptr [NeroComboBBuff::effect_stun], 0
        je dochain

        push eax
        push ecx
        mov eax, [edx+0xA4+0x28]   // mHitStopTimer (int)
        test eax, eax
        je skip_mul
        mov ecx, dword ptr [NeroComboBBuff::s_stun_int_factor]
        imul eax, ecx
        cmp eax, 32767
        jle store_val
        mov eax, 32767
        store_val:
        mov [edx+0xA4+0x28], eax
        skip_mul:
        pop ecx
        pop eax

        dochain:
        jmp [NeroComboBBuff::s_jmp_stun]    // → KB_detour → original
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool is_combo_b_move(uint32_t id) {
    for (int i = 0; i < 4; ++i) {
        if (NeroComboBBuff::cb_move_id[i] != 0 &&
            static_cast<uint32_t>(NeroComboBBuff::cb_move_id[i]) == id) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// on_frame — detection + effects 1, 2, 4
// ---------------------------------------------------------------------------
void NeroComboBBuff::on_frame(fmilliseconds& dt) {
    uPlayer* player = devil4_sdk::get_local_player();
    if (player == nullptr) return;

    // Always update readout so user can configure without enabling the mod.
    m_dbg_move_id    = player->moveIDBest;
    m_dbg_move_part  = player->movePart;
    m_dbg_is_nero    = (player->controllerID == 1);
    m_dbg_exceed_lvl = player->exceedLevel;
    m_dbg_exceed_tmr = player->exceedTimer;
    m_dbg_dt_active  = player->dtActive;

    // Sync asm-visible statics.
    s_stun_int_factor = stun_multiplier;

    if (!mod_enabled || !m_dbg_is_nero) {
        s_combo_b_active = false;
        m_active_frames  = 0;
        m_hit_count      = 0;
        m_launcher_fired = false;
        m_prev_move_id   = player->moveIDBest;
        m_prev_move_part = player->movePart;
        return;
    }

    // ---- Combo B detection ----
    const bool in_combo_b = is_combo_b_move(player->moveIDBest);
    if (in_combo_b) {
        m_active_frames = 30;
        s_combo_b_active = true;
    } else {
        if (m_active_frames > 0) {
            --m_active_frames;
        }
        if (m_active_frames == 0) {
            s_combo_b_active = false;
            m_hit_count      = 0;
            m_launcher_fired = false;
        }
    }

    // Rising edge on movePart → new hit within the combo.
    const bool move_id_changed  = (player->moveIDBest != m_prev_move_id);
    const bool move_part_raised = (player->movePart   >  m_prev_move_part);
    const bool new_hit = s_combo_b_active &&
                         (move_id_changed || move_part_raised) &&
                         (m_prev_move_id != 0);

    if (new_hit) {
        ++m_hit_count;

        // --- Effect 4: partial Exceed charge per hit ---
        if (effect_exceed && exceed_per_hit > 0.0f) {
            player->exceedTimer += exceed_per_hit;
            if (player->exceedTimer > 9.0f) player->exceedTimer = 9.0f;
        }
    }

    // --- Effect 1: suppress player hitstop every frame while in Combo B ---
    if (effect_no_hitstop && s_combo_b_active) {
        player->hitstop      = false;
        player->hitstopTimer = 0.0f;
    }

    // --- Effect 2: launcher on the 2nd+ hit ---
    if (effect_launcher && s_combo_b_active &&
        !m_launcher_fired &&
        player->movePart >= static_cast<uint32_t>(launcher_at_part) &&
        m_hit_count >= 2) {

        uEnemy* target = player->lockOnTargetPtr3;
        if (!target) target = player->lockOnTargetPtr1;
        if (!target) target = player->lockOnTargetPtr2;
        if (!target) target = player->lockOnTargetPtr4;
        if (target) {
            target->velocity.y = launcher_strength;
        }
        m_launcher_fired = true;
    }

    m_prev_move_id   = player->moveIDBest;
    m_prev_move_part = player->movePart;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
std::optional<std::string> NeroComboBBuff::on_initialize() {
    // Chain after KnockbackEdits (which hooks 0x10CA35 first). MinHook's second
    // hook on the same address creates the chain:
    //   nero_stun_detour → [s_jmp_stun = NCB trampoline = JMP KB_detour]
    //   → KB_detour → original.
    if (!install_hook_offset(0x10CA35, m_stun_hook,
                             reinterpret_cast<void*>(&nero_stun_detour),
                             &s_jmp_stun, 0)) {
        spdlog::error("NeroComboBBuff: failed to install stun hook at 0x10CA35. "
                      "Stun multiplier effect will be unavailable.");
        // Not fatal — the other three effects still work via on_frame.
    }
    return Mod::on_initialize();
}

NeroComboBBuff::~NeroComboBBuff() {
    m_stun_hook.reset();
    s_jmp_stun       = 0;
    s_combo_b_active = false;
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------
void NeroComboBBuff::on_gui_frame(int display) {
    if (display != 1) return;

    ImGui::Checkbox(_("Nero Combo B Buff"), &mod_enabled);
    ImGui::SameLine();
    help_marker(_(
        "Enhances Nero's Red Queen Combo B with four optional effects.\n\n"
        "SETUP: Perform Combo B and note the 'Live move ID' values for each "
        "hit, then enter them in the 'Combo B Move IDs' fields below. "
        "All effects gate on this detection — the mod does nothing while "
        "the IDs are 0.\n\n"
        "Effects (each has its own toggle):\n"
        "  1. No Hit-Stop — combo B can't be interrupted by recoil from "
        "shields or heavy targets.\n"
        "  2. 2nd-Hit Launcher — second hit launches the enemy upward.\n"
        "  3. Better Stagger — all hits apply a stun multiplier.\n"
        "  4. Better Exceed — each hit partially charges the Exceed gauge."));

    ImGui::Indent(lineIndent);

    // ----- Live readout (always visible) -----
    ImGui::Text(_("Move ID: 0x%04X   Part: %u   (%s)"),
                m_dbg_move_id, m_dbg_move_part,
                m_dbg_is_nero ? _("Nero") : _("not Nero"));
    ImGui::SameLine();
    help_marker(_("Perform Combo B and note the move ID(s) shown, then "
                  "enter them in the fields below."));

    ImGui::Text(_("Exceed: %u bars  timer: %.2f   DT: %s   Combo B: %s  hits: %d"),
                static_cast<uint32_t>(m_dbg_exceed_lvl),
                m_dbg_exceed_tmr,
                m_dbg_dt_active ? _("YES") : _("no"),
                s_combo_b_active ? _("ACTIVE") : _("--"),
                m_hit_count);

    ImGui::Separator();

    // ----- Detection config -----
    ImGui::Text(_("Combo B Move IDs (0 = unused)"));
    ImGui::PushItemWidth(120.0f);
    ImGui::InputInt(_("Hit 1##cb1"), &cb_move_id[0]); ImGui::SameLine();
    ImGui::InputInt(_("Hit 2##cb2"), &cb_move_id[1]); 
    ImGui::InputInt(_("Hit 3##cb3"), &cb_move_id[2]); ImGui::SameLine();
    ImGui::InputInt(_("Hit 4##cb4"), &cb_move_id[3]);
    ImGui::PopItemWidth();

    if (!mod_enabled) {
        ImGui::TextDisabled(_("(enable above to use effects)"));
        ImGui::Unindent(lineIndent);
        return;
    }

    ImGui::Separator();

    // ----- Effect 1: No Hit-Stop -----
    ImGui::Checkbox(_("No Hit-Stop Interruption"), &effect_no_hitstop);
    ImGui::SameLine();
    help_marker(_("While Combo B is active, Nero's hitstop and hitstopTimer "
                  "are forced to zero each frame. This prevents the recoil "
                  "freeze when hitting shields or heavy armoured enemies, "
                  "letting the combo continue uninterrupted."));

    ImGui::Separator();

    // ----- Effect 2: Launcher -----
    ImGui::Checkbox(_("2nd-Hit Launcher"), &effect_launcher);
    ImGui::SameLine();
    help_marker(_("On the second hit of Combo B, launches the lock-on target "
                  "upward. Fires once per combo sequence."));
    if (effect_launcher) {
        ImGui::Indent(lineIndent);
        ImGui::SliderFloat(_("Launch strength"), &launcher_strength,
                           0.0f, 30.0f, "%.1f");
        ImGui::InputInt(_("Trigger at movePart >="), &launcher_at_part);
        if (launcher_at_part < 0) launcher_at_part = 0;
        if (launcher_at_part > 20) launcher_at_part = 20;
        ImGui::SameLine();
        help_marker(_("movePart value at which the launcher fires. "
                      "Watch the 'Part' readout above during Combo B to "
                      "find the right value for the 2nd hit."));
        ImGui::Unindent(lineIndent);
    }

    ImGui::Separator();

    // ----- Effect 3: Stun multiplier -----
    ImGui::Checkbox(_("Better Stagger (stun multiplier)"), &effect_stun);
    ImGui::SameLine();
    help_marker(_("Multiplies the mHitStopTimer of every Combo B melee hit. "
                  "Higher values make enemies stagger longer and more "
                  "reliably on each strike. Requires the stun hook to be "
                  "installed (shown below)."));
    if (effect_stun) {
        ImGui::Indent(lineIndent);
        if (ImGui::SliderInt(_("Stun multiplier"), &stun_multiplier,
                             1, 50)) {
            if (stun_multiplier < 1)  stun_multiplier = 1;
            if (stun_multiplier > 50) stun_multiplier = 50;
            s_stun_int_factor = stun_multiplier;
        }
        ImGui::Text(_("Stun hook: %s"),
                    s_jmp_stun ? _("installed") : _("FAILED (effect inactive)"));
        ImGui::Unindent(lineIndent);
    }

    ImGui::Separator();

    // ----- Effect 4: Exceed charge per hit -----
    ImGui::Checkbox(_("Better Exceed (charge per hit)"), &effect_exceed);
    ImGui::SameLine();
    help_marker(_("Each Combo B hit adds a chunk directly to Nero's partial "
                  "Exceed charge (exceedTimer). The bars fill naturally once "
                  "the timer exceeds the game's internal threshold."));
    if (effect_exceed) {
        ImGui::Indent(lineIndent);
        ImGui::SliderFloat(_("Exceed per hit"), &exceed_per_hit,
                           0.0f, 5.0f, "%.2f");
        ImGui::SameLine();
        help_marker(_("How much is added to exceedTimer on each Combo B hit. "
                      "Default 0.5. Values above ~3.0 will typically fill an "
                      "entire bar per hit."));
        ImGui::Unindent(lineIndent);
    }

    ImGui::Unindent(lineIndent);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
void NeroComboBBuff::on_config_load(const utility::Config& cfg) {
    mod_enabled       = cfg.get<bool> ("nero_cb_enabled").value_or(false);
    cb_move_id[0]     = cfg.get<int>  ("nero_cb_move0").value_or(0);
    cb_move_id[1]     = cfg.get<int>  ("nero_cb_move1").value_or(0);
    cb_move_id[2]     = cfg.get<int>  ("nero_cb_move2").value_or(0);
    cb_move_id[3]     = cfg.get<int>  ("nero_cb_move3").value_or(0);
    launcher_at_part  = cfg.get<int>  ("nero_cb_launch_part").value_or(1);
    if (launcher_at_part < 0)  launcher_at_part = 0;
    if (launcher_at_part > 20) launcher_at_part = 20;

    effect_no_hitstop = cfg.get<bool> ("nero_cb_fx_hitstop").value_or(true);
    effect_launcher   = cfg.get<bool> ("nero_cb_fx_launcher").value_or(true);
    effect_stun       = cfg.get<bool> ("nero_cb_fx_stun").value_or(true);
    effect_exceed     = cfg.get<bool> ("nero_cb_fx_exceed").value_or(true);

    launcher_strength = cfg.get<float>("nero_cb_launch_str").value_or(8.0f);

    stun_multiplier   = cfg.get<int>  ("nero_cb_stun_mul").value_or(20);
    if (stun_multiplier < 1)  stun_multiplier = 1;
    if (stun_multiplier > 50) stun_multiplier = 50;
    s_stun_int_factor = stun_multiplier;

    exceed_per_hit    = cfg.get<float>("nero_cb_exceed_hit").value_or(0.5f);
}

void NeroComboBBuff::on_config_save(utility::Config& cfg) {
    cfg.set<bool> ("nero_cb_enabled",     mod_enabled);
    cfg.set<int>  ("nero_cb_move0",       cb_move_id[0]);
    cfg.set<int>  ("nero_cb_move1",       cb_move_id[1]);
    cfg.set<int>  ("nero_cb_move2",       cb_move_id[2]);
    cfg.set<int>  ("nero_cb_move3",       cb_move_id[3]);
    cfg.set<int>  ("nero_cb_launch_part", launcher_at_part);
    cfg.set<bool> ("nero_cb_fx_hitstop",  effect_no_hitstop);
    cfg.set<bool> ("nero_cb_fx_launcher", effect_launcher);
    cfg.set<bool> ("nero_cb_fx_stun",     effect_stun);
    cfg.set<bool> ("nero_cb_fx_exceed",   effect_exceed);
    cfg.set<float>("nero_cb_launch_str",  launcher_strength);
    cfg.set<int>  ("nero_cb_stun_mul",    stun_multiplier);
    cfg.set<float>("nero_cb_exceed_hit",  exceed_per_hit);
}
