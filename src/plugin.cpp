#include "plugin.h"
#include "tracer.h"
#include "dialog.h"

enum MenuEntries
{
    MENU_SHOW_DIALOG = 0,
    MENU_START_TRACE,
    MENU_STOP_TRACE,
};

static void cbMenuEntry(CBTYPE, void* callbackInfo)
{
    auto info = (PLUG_CB_MENUENTRY*)callbackInfo;
    switch (info->hEntry)
    {
    case MENU_SHOW_DIALOG:
        DialogShow(GuiGetWindowHandle());
        break;
    case MENU_START_TRACE:
        Tracer::Instance().Start();
        break;
    case MENU_STOP_TRACE:
        Tracer::Instance().Stop();
        break;
    }
}

static void cbBreakpoint(CBTYPE, void* callbackInfo)
{
    Tracer::Instance().OnBreakpoint();
}

static void cbInitDebug(CBTYPE, void* callbackInfo)
{
    DialogRefreshModules();
}

static void cbStopDebug(CBTYPE, void* callbackInfo)
{
    Tracer::Instance().Stop();
    DialogOnDebugStop();
}

bool pluginInit(PLUG_INITSTRUCT* initStruct)
{
    dprintf("pluginInit(pluginHandle: %d)\n", pluginHandle);

    _plugin_registercallback(pluginHandle, CB_MENUENTRY, cbMenuEntry);
    _plugin_registercallback(pluginHandle, CB_BREAKPOINT, cbBreakpoint);
    _plugin_registercallback(pluginHandle, CB_INITDEBUG, cbInitDebug);
    _plugin_registercallback(pluginHandle, CB_STOPDEBUG, cbStopDebug);
    _plugin_registercallback(pluginHandle, CB_TRACEEXECUTE, Tracer::cbTraceStep);
    _plugin_registercallback(pluginHandle, CB_STEPPED, Tracer::cbStepped);
    _plugin_registercallback(pluginHandle, CB_DEBUGEVENT, Tracer::cbDebugEvent);
    _plugin_registercallback(pluginHandle, CB_EXCEPTION, Tracer::cbException);

    return true;
}

void pluginStop()
{
    Tracer::Instance().Stop();
    DialogDestroy();
    dprintf("pluginStop(pluginHandle: %d)\n", pluginHandle);
}

void pluginSetup()
{
    _plugin_menuaddentry(hMenu, MENU_SHOW_DIALOG, "Extended Trace Window");
    _plugin_menuaddseparator(hMenu);
    _plugin_menuaddentry(hMenu, MENU_START_TRACE, "Start Trace");
    _plugin_menuaddentry(hMenu, MENU_STOP_TRACE, "Finish Trace");

    dprintf("pluginSetup(pluginHandle: %d)\n", pluginHandle);
}
