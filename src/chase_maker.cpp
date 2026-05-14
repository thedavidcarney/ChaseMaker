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
#include "chase_maker_build_stamp.h"   // CM_BUILD_STAMP

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
        A_Err err = A_Err_NONE;

        // Acquire the panel suite (AEGP_PanelSuite1, frozen in AE 8.0).
        // AcquireSuite is a C function pointer — no implicit `this`.
        // C-style cast on the out param: reinterpret_cast can't add the
        // const through two indirections (AEGP_PanelSuite1** -> const void**).
        err = i_pica_basicP->AcquireSuite(
            kAEGPPanelSuite,
            kAEGPPanelSuiteVersion1,
            (const void**)&i_panel_suiteP);
        if (err != A_Err_NONE || !i_panel_suiteP) throw err;

        // Reserve a unique command ID and bolt it onto the Window menu.
        err = i_sp.CommandSuite1()->AEGP_GetUniqueCommand(&i_command);
        if (err != A_Err_NONE) throw err;

        err = i_sp.CommandSuite1()->AEGP_InsertMenuCommand(
            i_command,
            kChaseMakerMenuLabel,
            AEGP_Menu_WINDOW,
            AEGP_MENU_INSERT_SORTED);
        if (err != A_Err_NONE) throw err;

        // Hook the command (toggle visibility) and the menu update
        // (keep the check mark in sync with the panel state).
        err = i_sp.RegisterSuite5()->AEGP_RegisterCommandHook(
            i_plugin_id,
            AEGP_HP_BeforeAE,
            i_command,
            &ChaseMakerPlugin::S_CommandHook,
            reinterpret_cast<AEGP_CommandRefcon>(this));
        if (err != A_Err_NONE) throw err;

        err = i_sp.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(
            i_plugin_id,
            &ChaseMakerPlugin::S_UpdateMenuHook,
            nullptr);
        if (err != A_Err_NONE) throw err;

        // Tell AE we own a panel under this match name. AE will call
        // S_CreatePanelHook the first time the panel becomes visible.
        err = i_panel_suiteP->AEGP_RegisterCreatePanelHook(
            i_plugin_id,
            kChaseMakerMatchName,
            &ChaseMakerPlugin::S_CreatePanelHook,
            reinterpret_cast<AEGP_CreatePanelRefcon>(this),
            true);
        if (err != A_Err_NONE) throw err;
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
        PanelRenderer* renderer = CreatePanelRenderer(
            reinterpret_cast<void*>(container),
            self ? &self->i_panel_state : nullptr);

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
    try {
        *global_refconP = reinterpret_cast<AEGP_GlobalRefcon>(
            new ChaseMakerPlugin(pica_basicP, aegp_plugin_id));
    } catch (A_Err err) {
        return err;
    } catch (std::bad_alloc&) {
        return A_Err_ALLOC;
    } catch (...) {
        return A_Err_GENERIC;
    }
    return A_Err_NONE;
}
