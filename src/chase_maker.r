#include "AEConfig.h"

#ifndef AE_OS_WIN
    #include "AE_General.r"
#endif

// AEGP panel plugin PiPL. Mirrors the SDK Panelator sample's shape;
// `Kind = AEGP` is what tells AE this is a general plugin (not a PF
// effect). Panel registration itself happens at runtime in
// EntryPointFunc via AEGP_PanelSuite1.
resource 'PiPL' (16000) {
    {
        Kind {
            AEGP
        },
        // Shown in About Plug-ins; not the Window-menu label (that's
        // set at runtime via AEGP_InsertMenuCommand).
        Name {
            "Chase Maker"
        },
        Category {
            "General Plugin"
        },
        // Version 0.1.0 packed as (major << 16) | (minor << 8) | bug.
        Version {
            256
        },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
        CodeWin64X86 {"EntryPointFunc"},
    #elif defined(AE_PROC_ARM64)
        CodeWinARM64 {"EntryPointFunc"},
    #endif
#elif defined(AE_OS_MAC)
        CodeMacIntel64 {"EntryPointFunc"},
        CodeMacARM64 {"EntryPointFunc"},
#endif
    }
};
