// Events the manager sends to addons (IHost::SubscribeEvent), and the payloads that come with them. The numbers are part of the addon API: they
// never change, and new events only take new numbers.
#pragma once
#include <cstdint>

enum EamEvent : uint32_t {
    EAM_EVENT_ADDON_LOADED       = 1,   // an addon has started; payload EamAddonEventData
    EAM_EVENT_ADDON_UNLOADED     = 2,   // an addon has stopped (switched off, or unloaded); payload EamAddonEventData
    EAM_EVENT_SETTINGS_CHANGED   = 3,   // reserved: not sent (yet)
    EAM_EVENT_SHADER_INTERCEPTED = 4,   // reserved: not sent (yet); its payload would be EamShaderEventData
    EAM_EVENT_HOST_SHUTDOWN      = 5,   // Lossless Scaling is closing; no payload
    EAM_EVENT_SETTINGS_APPLIED   = 6,   // Lossless Scaling has just applied its settings (its ApplySettings returned); no payload
    EAM_EVENT_D3D11_DEVICE_READY = 7,   // Lossless Scaling made a D3D11 device (IHost::GetD3D11Device has it now); no payload
    EAM_EVENT_D3D11_DEVICE_CHANGED = 8, // ...and it replaces an earlier one: drop anything made on the old device; sent before DEVICE_READY
    EAM_EVENT_REPORT             = 11,  // a report is being made (the Super log feature; API 1.4): write your own files (a trace, your state) into the folder; payload EamReportData; called before the zip is made
    EAM_EVENT_SCALING_STATUS     = 10,  // Lossless Scaling's scaling started, stopped or changed (API 1.4); payload EamScalingStatusData
    EAM_EVENT_RECORDING_SAVED    = 9,   // the manager's recorder saved a file (API 1.3); payload EamRecordingSavedData: an addon can write what it knows beside it

    EAM_EVENT_CUSTOM             = 0x10000   // from here up: addons' own events, for talking to each other
};

// Called on the thread that published the event. `data` is valid only during the call.
typedef void (*EamEventCallback)(uint32_t eventId, const void* data, uint32_t dataSize, void* userData);

struct EamAddonEventData {
    const char* addonName;
    const char* addonVersion;
};

// What Lossless Scaling tells its window when scaling changes (API 1.4). status: 0 disabled, 1 temporarily disabled, 2 enabled (scaling runs), 3 a countdown to start. hwnd is the
// window being scaled; the sizes are the captured and the output picture in pixels; resized: the window changed size; errorCode: 0 none, 1 resize failed, 2 offset too large, 3 capture failed.
struct EamScalingStatusData {
    int32_t status;
    void* hwnd;
    int32_t inputWidth, inputHeight, outputWidth, outputHeight;
    uint8_t resized;
    float scaleFactor;
    int32_t errorCode;
};

struct EamReportData {
    const wchar_t* folder;   // the folder that becomes the report: put files in it (short names; the addon's id in the name), then return
    const char* reason;      // why the report is made ("hotkey", "button", or the error that started it)
};

struct EamRecordingSavedData {
    const wchar_t* path;    // the .lsrec file that was written
};

struct EamShaderEventData {
    const wchar_t* resourceName;
    const wchar_t* resourceType;
    uint32_t dataSize;
};
