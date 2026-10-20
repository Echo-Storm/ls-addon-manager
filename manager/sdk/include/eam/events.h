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
    EAM_EVENT_RECORDING_SAVED    = 9,   // the manager's recorder saved a file (API 1.3); payload EamRecordingSavedData: an addon can write what it knows beside it

    EAM_EVENT_CUSTOM             = 0x10000   // from here up: addons' own events, for talking to each other
};

// Called on the thread that published the event. `data` is valid only during the call.
typedef void (*EamEventCallback)(uint32_t eventId, const void* data, uint32_t dataSize, void* userData);

struct EamAddonEventData {
    const char* addonName;
    const char* addonVersion;
};

struct EamRecordingSavedData {
    const wchar_t* path;    // the .lsrec file that was written
};

struct EamShaderEventData {
    const wchar_t* resourceName;
    const wchar_t* resourceType;
    uint32_t dataSize;
};
