// Chase Maker — AEGP panel plugin.
//
// Bite 3 / hello-world: register a panel named "Chase Maker" under
// AE's Window menu. Clicking it toggles visibility. The panel itself
// is empty — drawing/UI lands in a later bite once the UI framework
// (probably Dear ImGui) is wired in.
//
// Shape mirrors the SDK's Panelator sample, stripped of its flyout
// menu, RGB demo, and platform child-window. The runtime panel match
// name is "tdcarney Chase Maker" (20 chars, under PF_MAX_EFFECT_NAME's
// 31-char cap, mirrors EXRDemux's `tdcarney EXRDemux`). Don't change
// it after the first release — saved AE projects will key off it.

#include "AEConfig.h"
#ifdef AE_OS_WIN
    #include <windows.h>
#endif

#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AE_GeneralPlugPanels.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"

#include "panel_renderer.h"
#include "panel_state.h"
#include "ae_build.h"
#include "chase_maker_build_stamp.h"   // CM_BUILD_STAMP
#include "diag_log.h"                  // CM_DIAG_LOG load-path tracing

#include <cstdio>

// Match name (panel-suite calls): A_u_char* / UTF-8 byte string.
// Menu label (command-suite calls): A_char* / signed char.
// The SDK signs them differently so we keep two typed handles.
static const A_u_char* const kChaseMakerMatchName =
    reinterpret_cast<const A_u_char*>("tdcarney Chase Maker");

static const A_char* const kChaseMakerMenuLabel = "Chase Maker";

namespace {

// AE's panel function table requires three callbacks. For the
// hello-world panel, all three are no-ops — AE handles defaults
// when we don't populate snap sizes or a flyout menu.

A_Err PanelGetSnapSizes(AEGP_PanelRefcon /*refcon*/,
                        A_LPoint* /*snapSizes*/,
                        A_long* numSizesP)
{
    if (numSizesP) *numSizesP = 0;
    return A_Err_NONE;
}

A_Err PanelPopulateFlyout(AEGP_PanelRefcon /*refcon*/,
                          AEGP_FlyoutMenuItem* /*itemsP*/,
                          A_long* in_out_numItemsP)
{
    if (in_out_numItemsP) *in_out_numItemsP = 0;
    return A_Err_NONE;
}

A_Err PanelDoFlyoutCommand(AEGP_PanelRefcon /*refcon*/,
                           AEGP_FlyoutMenuCmdID /*commandID*/)
{
    return A_Err_NONE;
}

class ChaseMakerPlugin
{
public:
    ChaseMakerPlugin(SPBasicSuite* pica_basicP, AEGP_PluginID plugin_id)
        : i_pica_basicP(pica_basicP),
          i_plugin_id(plugin_id),
          i_sp(pica_basicP),
          i_panel_suiteP(nullptr),
          i_command(0)
    {
        CM_DIAG_LOG("ctor: begin");
        A_Err err = A_Err_NONE;
        // Stash the SPBasicSuite + plugin_id on PanelState so the
        // ae_build module can re-acquire AEGP suites later without
        // routing back through the plugin object.
        i_panel_state.pica_basicP    = pica_basicP;
        i_panel_state.aegp_plugin_id = static_cast<int>(plugin_id);

        // Acquire the panel suite (AEGP_PanelSuite1, frozen in AE 8.0).
        // AcquireSuite is a C function pointer — no implicit `this`.
        // C-style cast on the out param: reinterpret_cast can't add the
        // const through two indirections (AEGP_PanelSuite1** -> const void**).
        err = i_pica_basicP->AcquireSuite(
            kAEGPPanelSuite,
            kAEGPPanelSuiteVersion1,
            (const void**)&i_panel_suiteP);
        if (err != A_Err_NONE || !i_panel_suiteP) throw err;
        CM_DIAG_LOG("ctor: panel suite acquired");

        // Reserve a unique command ID and bolt it onto the Window menu.
        err = i_sp.CommandSuite1()->AEGP_GetUniqueCommand(&i_command);
        if (err != A_Err_NONE) throw err;

        err = i_sp.CommandSuite1()->AEGP_InsertMenuCommand(
            i_command,
            kChaseMakerMenuLabel,
            AEGP_Menu_WINDOW,
            AEGP_MENU_INSERT_SORTED);
        if (err != A_Err_NONE) throw err;
        CM_DIAG_LOG("ctor: menu command inserted");

        // Hook the command (toggle visibility) and the menu update
        // (keep the check mark in sync with the panel state).
        err = i_sp.RegisterSuite5()->AEGP_RegisterCommandHook(
            i_plugin_id,
            AEGP_HP_BeforeAE,
            i_command,
            &ChaseMakerPlugin::S_CommandHook,
            reinterpret_cast<AEGP_CommandRefcon>(this));
        if (err != A_Err_NONE) throw err;
        CM_DIAG_LOG("ctor: command hook registered");

        err = i_sp.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(
            i_plugin_id,
            &ChaseMakerPlugin::S_UpdateMenuHook,
            nullptr);
        if (err != A_Err_NONE) throw err;
        CM_DIAG_LOG("ctor: update-menu hook registered");

        // Idle hook — AE calls this periodically with a valid AEGP
        // context. We drain the panel's deferred build flags here so
        // ae_build's AEGP calls (project/folder/comp/footage queries)
        // run from inside an AE-registered hook. Calling AEGP from
        // the panel's render thread directly returns "num_proj=0"
        // because AE doesn't expose the project state outside hooks.
        err = i_sp.RegisterSuite5()->AEGP_RegisterIdleHook(
            i_plugin_id,
            &ChaseMakerPlugin::S_IdleHook,
            nullptr);
        if (err != A_Err_NONE) throw err;
        CM_DIAG_LOG("ctor: idle hook registered");

        // Tell AE we own a panel under this match name. AE will call
        // S_CreatePanelHook the first time the panel becomes visible.
        err = i_panel_suiteP->AEGP_RegisterCreatePanelHook(
            i_plugin_id,
            kChaseMakerMatchName,
            &ChaseMakerPlugin::S_CreatePanelHook,
            reinterpret_cast<AEGP_CreatePanelRefcon>(this),
            true);
        if (err != A_Err_NONE) throw err;
        CM_DIAG_LOG("ctor: create-panel hook registered; ctor done");
    }

private:
    SPBasicSuite*       i_pica_basicP;
    AEGP_PluginID       i_plugin_id;
    AEGP_SuiteHandler   i_sp;
    AEGP_PanelSuite1*   i_panel_suiteP;
    AEGP_Command        i_command;

public:
    // PanelState lives on the plugin global so it survives a panel
    // close/reopen within an AE session. The renderer borrows a
    // pointer to it via CreatePanelRenderer.
    PanelState          i_panel_state;

private:

    static SPAPI A_Err S_CommandHook(
        AEGP_GlobalRefcon /*plugin_refcon*/,
        AEGP_CommandRefcon refcon,
        AEGP_Command command,
        AEGP_HookPriority /*hook_priority*/,
        A_Boolean /*already_handled*/,
        A_Boolean* handledPB)
    {
        auto* self = reinterpret_cast<ChaseMakerPlugin*>(refcon);
        if (!self || command != self->i_command) {
            if (handledPB) *handledPB = FALSE;
            return A_Err_NONE;
        }
        A_Err err = self->i_panel_suiteP->AEGP_ToggleVisibility(kChaseMakerMatchName);
        if (handledPB) *handledPB = TRUE;
        return err;
    }

    static A_Err S_IdleHook(
        AEGP_GlobalRefcon plugin_refcon,
        AEGP_IdleRefcon /*refcon*/,
        A_long* max_sleepPL)
    {
        auto* self = reinterpret_cast<ChaseMakerPlugin*>(plugin_refcon);
        if (!self) return A_Err_NONE;

        static bool s_first_idle = true;
        if (s_first_idle) {
            s_first_idle = false;
            CM_DIAG_LOG("S_IdleHook: first idle tick reached "
                        "(AE is past load and pumping)");
        }

        // Drain UI-side build requests. ae_build's AEGP calls (proj /
        // item / comp / footage / effect) all succeed from this
        // hook's context. If nothing's pending we just return; AE
        // will call us again soon.
        bool did_build = false;
        if (int idx = self->i_panel_state.want_build_chase_index.exchange(-1);
            idx >= 0)
        {
            ae_build::BuildChase(&self->i_panel_state, idx);
            did_build = true;
        }
        if (self->i_panel_state.want_build_all_chases.exchange(false)) {
            ae_build::BuildAllChases(&self->i_panel_state);
            did_build = true;
        }
        // Keep the chase preview's fps in sync with the active comp —
        // but THROTTLED and guarded. Polling AEGP project/comp state
        // every idle (~10Hz, unconditionally) destabilised AE while it
        // was doing its own work: DBSync DeserializeFullProject crash,
        // RenderTaskManager crash, and an MP-off render deadlock all
        // traced to this. fps changes ~never, so poll ~every 3s and
        // never on a tick where we just drove a build (AE is busy).
        static int s_fps_tick = 0;
        if (!did_build && ++s_fps_tick >= 30) {
            s_fps_tick = 0;
            CM_DIAG_LOG("idle: RefreshProjectFps begin");
            ae_build::RefreshProjectFps(&self->i_panel_state);
            CM_DIAG_LOG("idle: RefreshProjectFps end");
        }
        // max_sleep is in 60ths of a second. Asking AE to wake us at
        // up to 6/60s = 100ms keeps latency low between UI click and
        // build start without burning idle CPU.
        if (max_sleepPL && *max_sleepPL > 6) *max_sleepPL = 6;
        return A_Err_NONE;
    }

    static A_Err S_UpdateMenuHook(
        AEGP_GlobalRefcon plugin_refcon,
        AEGP_UpdateMenuRefcon /*refcon*/,
        AEGP_WindowType /*active_window*/)
    {
        auto* self = reinterpret_cast<ChaseMakerPlugin*>(plugin_refcon);
        if (!self) return A_Err_NONE;

        A_Err err = self->i_sp.CommandSuite1()->AEGP_EnableCommand(self->i_command);
        if (err != A_Err_NONE) return err;

        A_Boolean shownB = FALSE;
        A_Boolean frontmostB = FALSE;
        err = self->i_panel_suiteP->AEGP_IsShown(kChaseMakerMatchName, &shownB, &frontmostB);
        if (err != A_Err_NONE) return err;

        return self->i_sp.CommandSuite1()->AEGP_CheckMarkMenuCommand(
            self->i_command, shownB && frontmostB);
    }

    static A_Err S_CreatePanelHook(
        AEGP_GlobalRefcon plugin_refcon,
        AEGP_CreatePanelRefcon /*refcon*/,
        AEGP_PlatformViewRef container,
        AEGP_PanelH panelH,
        AEGP_PanelFunctions1* outFunctionTable,
        AEGP_PanelRefcon* outRefcon)
    {
        if (outFunctionTable) {
            outFunctionTable->GetSnapSizes    = PanelGetSnapSizes;
            outFunctionTable->PopulateFlyout  = PanelPopulateFlyout;
            outFunctionTable->DoFlyoutCommand = PanelDoFlyoutCommand;
        }

        CM_DIAG_LOG("S_CreatePanelHook: enter (container=%p, panelH=%p)",
                    (void*)container, (void*)panelH);
        auto* self = reinterpret_cast<ChaseMakerPlugin*>(plugin_refcon);

        // Override the panel's tab label so users see "Chase Maker"
        // (plus a build stamp) instead of the internal match name
        // ("tdcarney Chase Maker"). Build stamp uses the source's
        // compile time so different rebuilds are visually distinct
        // — handy when iterating quickly and verifying which build
        // is actually loaded.
        if (self && self->i_panel_suiteP) {
            char title[128];
            std::snprintf(title, sizeof(title),
                "Chase Maker  (build %s)", CM_BUILD_STAMP);
            self->i_panel_suiteP->AEGP_SetTitle(panelH,
                reinterpret_cast<const A_u_char*>(title));
        }

        // Hand the platform container to the renderer factory along
        // with a pointer to the plugin-owned PanelState. The renderer
        // borrows the state; ownership stays here so user data (scan
        // results, exclusion choices, preview state) survives a
        // panel close/reopen — AE may destroy the platform view and
        // call this hook again with a fresh one.
        CM_DIAG_LOG("S_CreatePanelHook: calling CreatePanelRenderer");
        PanelRenderer* renderer = CreatePanelRenderer(
            reinterpret_cast<void*>(container),
            self ? &self->i_panel_state : nullptr);
        CM_DIAG_LOG("S_CreatePanelHook: CreatePanelRenderer returned %p",
                    (void*)renderer);

        if (outRefcon) {
            *outRefcon = reinterpret_cast<AEGP_PanelRefcon>(renderer);
        }
        return A_Err_NONE;
    }
};

} // namespace

// PiPL's CodeWin64X86 / CodeMacARM64 / etc. entries name this symbol.
// AE calls it exactly once at plugin load time.
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;

A_Err EntryPointFunc(
    SPBasicSuite*       pica_basicP,
    A_long              /*major_versionL*/,
    A_long              /*minor_versionL*/,
    AEGP_PluginID       aegp_plugin_id,
    AEGP_GlobalRefcon*  global_refconP)
{
    CM_DIAG_LOG("################ SESSION START  build=%s ################",
                CM_BUILD_STAMP);
    CM_DIAG_LOG("EntryPointFunc: enter (pica=%p, plugin_id=%ld)",
                (void*)pica_basicP, (long)aegp_plugin_id);
    try {
        *global_refconP = reinterpret_cast<AEGP_GlobalRefcon>(
            new ChaseMakerPlugin(pica_basicP, aegp_plugin_id));
    } catch (A_Err err) {
        CM_DIAG_LOG("EntryPointFunc: ChaseMakerPlugin ctor threw A_Err=%ld",
                    (long)err);
        return err;
    } catch (std::bad_alloc&) {
        CM_DIAG_LOG("EntryPointFunc: ctor threw bad_alloc");
        return A_Err_ALLOC;
    } catch (...) {
        CM_DIAG_LOG("EntryPointFunc: ctor threw unknown");
        return A_Err_GENERIC;
    }
    CM_DIAG_LOG("EntryPointFunc: returning A_Err_NONE (plugin constructed)");
    return A_Err_NONE;
}
