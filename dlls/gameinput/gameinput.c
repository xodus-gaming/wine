/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "devpropdef.h"
#include "devfiltertypes.h"
#include "devquery.h"
#include "hidusage.h"
#include "ddk/hidsdi.h"

#include "initguid.h"
#include "gameinput.h"
#include "devpkey.h"
#include "ddk/hidclass.h"

#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ginput);

DEFINE_GUID( GUID_DEVINTERFACE_WINEXINPUT,0x6c53d5fd,0x6480,0x440f,0xb6,0x18,0x47,0x67,0x50,0xc5,0xe1,0xa6 );

static CRITICAL_SECTION game_input_cs;
static CRITICAL_SECTION_DEBUG game_input_cs_debug =
{
    0, 0, &game_input_cs,
    { &game_input_cs_debug.ProcessLocksList, &game_input_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": game_input_cs") }
};
static CRITICAL_SECTION game_input_cs = { &game_input_cs_debug, -1, 0, 0, 0, 0 };
static struct game_input *game_input;

struct device
{
    IGameInputDevice_v0 IGameInputDevice_v0_iface;
    IGameInputDevice_v2 IGameInputDevice_v2_iface;
    LONG refcount;
    WCHAR path[MAX_PATH];
    struct list entry;

    GameInputDeviceStatus status;
    GameInputDeviceInfo_v0 info_v0;
    GameInputDeviceInfo_v2 info_v2;
    GameInputMouseInfo_v0 mouse_info_v0;
    GameInputMouseInfo_v2 mouse_info_v2;
};

static struct device *device_from_IGameInputDevice_v0( IGameInputDevice_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct device, IGameInputDevice_v0_iface );
}

static struct device *device_from_IGameInputDevice_v2( IGameInputDevice_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct device, IGameInputDevice_v2_iface );
}

static ULONG device_addref( struct device *device )
{
    ULONG ref = InterlockedIncrement( &device->refcount );
    TRACE( "device %p increasing refcount to %lu.\n", device, ref );
    return ref;
}

static ULONG device_release( struct device *device )
{
    ULONG ref = InterlockedDecrement( &device->refcount );
    TRACE( "device %p decreasing refcount to %lu.\n", device, ref );
    if (!ref) free( device );
    return ref;
}

static HRESULT device_query_interface( struct device *device, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IGameInputDevice_v0 ))
        *out = &device->IGameInputDevice_v0_iface;
    else if (IsEqualGUID( iid, &IID_IGameInputDevice_v2 ))
        *out = &device->IGameInputDevice_v2_iface;
    else
        return E_NOINTERFACE;

    device_addref( device );
    return S_OK;
}

static HRESULT WINAPI game_input_device_v0_QueryInterface( IGameInputDevice_v0 *iface, REFIID iid, void **out )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );

    TRACE( "device %p, iid %s, out %p.\n", device, debugstr_guid( iid ), out );

    return device_query_interface( device, iid, out );
}

static ULONG WINAPI game_input_device_v0_AddRef( IGameInputDevice_v0 *iface )
{
    return device_addref( device_from_IGameInputDevice_v0( iface ) );
}

static ULONG WINAPI game_input_device_v0_Release( IGameInputDevice_v0 *iface )
{
    return device_release( device_from_IGameInputDevice_v0( iface ) );
};

static const GameInputDeviceInfo_v0 *WINAPI game_input_device_v0_GetDeviceInfo( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    FIXME( "device %p stub!\n", device );
    return &device->info_v0;
}

static GameInputDeviceStatus WINAPI game_input_device_v0_GetDeviceStatus( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    TRACE( "device %p.\n", device );
    return device->status;
}

static void WINAPI game_input_device_v0_GetBatteryState( IGameInputDevice_v0 *iface, GameInputBatteryState *state )
{
    TRACE( "device %p, state %p.\n", device_from_IGameInputDevice_v0( iface ), state );
    if (!state) return;
    memset( state, 0, sizeof(*state) );
    state->status = GameInputBatteryNotPresent;
}

static HRESULT WINAPI game_input_device_v0_CreateForceFeedbackEffect( IGameInputDevice_v0 *iface, uint32_t index, const GameInputForceFeedbackParams *params,
                                                                  IGameInputForceFeedbackEffect_v0 **effect )
{
    FIXME( "device %p, index %u, params %p, effect %p stub!\n", device_from_IGameInputDevice_v0( iface ), index, params, effect );
    return E_NOTIMPL;
}

static bool WINAPI game_input_device_v0_IsForceFeedbackMotorPoweredOn( IGameInputDevice_v0 *iface, uint32_t index )
{
    FIXME( "device %p, index %u stub!\n", device_from_IGameInputDevice_v0( iface ), index );
    return FALSE;
}

static void WINAPI game_input_device_v0_SetForceFeedbackMotorGain( IGameInputDevice_v0 *iface, uint32_t index, float gain )
{
    FIXME( "device %p, index %u, gain %f stub!\n", device_from_IGameInputDevice_v0( iface ), index, gain );
}

static HRESULT WINAPI game_input_device_v0_SetHapticMotorState( IGameInputDevice_v0 *iface, uint32_t index, const GameInputHapticFeedbackParams *params )
{
    FIXME( "device %p, index %u, params %p stub!\n", device_from_IGameInputDevice_v0( iface ), index, params );
    return E_NOTIMPL;
}

static void WINAPI game_input_device_v0_SetRumbleState( IGameInputDevice_v0 *iface, const GameInputRumbleParams *params )
{
    FIXME( "device %p, params %p stub!\n", device_from_IGameInputDevice_v0( iface ), params );
}

static void WINAPI game_input_device_v0_SetInputSynchronizationState( IGameInputDevice_v0 *iface, bool enabled )
{
    FIXME( "device %p, enabled %d stub!\n", device_from_IGameInputDevice_v0( iface ), enabled );
}

static void WINAPI game_input_device_v0_SendInputSynchronizationHint( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
}

static void WINAPI game_input_device_v0_PowerOff( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
}

static HRESULT WINAPI game_input_device_v0_CreateRawDeviceReport( IGameInputDevice_v0 *iface, uint32_t report_id, GameInputRawDeviceReportKind report_kind,
                                                              IGameInputRawDeviceReport_v0 **report )
{
    FIXME( "device %p, report_id %u, report_kind %#x, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report_id, report_kind, report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_GetRawDeviceFeature( IGameInputDevice_v0 *iface, uint32_t report_id, IGameInputRawDeviceReport_v0 **report )
{
    FIXME( "device %p, report_id %u, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report_id, report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_SetRawDeviceFeature( IGameInputDevice_v0 *iface, IGameInputRawDeviceReport_v0 *report )
{
    FIXME( "device %p, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_SendRawDeviceOutput( IGameInputDevice_v0 *iface, IGameInputRawDeviceReport_v0 *report )
{
    FIXME( "device %p, report %p stub!\n", device_from_IGameInputDevice_v0( iface ), report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_SendRawDeviceOutputWithResponse( IGameInputDevice_v0 *iface, IGameInputRawDeviceReport_v0 *request_report,
                                                                        IGameInputRawDeviceReport_v0 **response_report )
{
    FIXME( "device %p, request_report %p, response_report %p stub!\n", device_from_IGameInputDevice_v0( iface ), request_report, response_report );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_device_v0_ExecuteRawDeviceIoControl( IGameInputDevice_v0 *iface, uint32_t control_code, size_t input_buffer_size, const void *input_buffer,
                                                                  size_t output_buffer_size, void *output_buffer, size_t *output_size )
{
    FIXME( "device %p, control_code %u, input_buffer_size %Iu, input_buffer %p, output_buffer_size %Iu, output_buffer %p, output_size %p stub!\n",
           device_from_IGameInputDevice_v0( iface ), control_code, input_buffer_size, input_buffer, output_buffer_size, output_buffer, output_size );
    return E_NOTIMPL;
}

static bool WINAPI game_input_device_v0_AcquireExclusiveRawDeviceAccess( IGameInputDevice_v0 *iface, uint64_t timeout_us )
{
    FIXME( "device %p, timeout_us %I64u stub!\n", device_from_IGameInputDevice_v0( iface ), timeout_us );
    return FALSE;
}

static void WINAPI game_input_device_v0_ReleaseExclusiveRawDeviceAccess( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
}

static const IGameInputDevice_v0Vtbl game_input_device_v0_vtbl =
{
    game_input_device_v0_QueryInterface,
    game_input_device_v0_AddRef,
    game_input_device_v0_Release,
    game_input_device_v0_GetDeviceInfo,
    game_input_device_v0_GetDeviceStatus,
    game_input_device_v0_GetBatteryState,
    game_input_device_v0_CreateForceFeedbackEffect,
    game_input_device_v0_IsForceFeedbackMotorPoweredOn,
    game_input_device_v0_SetForceFeedbackMotorGain,
    game_input_device_v0_SetHapticMotorState,
    game_input_device_v0_SetRumbleState,
    game_input_device_v0_SetInputSynchronizationState,
    game_input_device_v0_SendInputSynchronizationHint,
    game_input_device_v0_PowerOff,
    game_input_device_v0_CreateRawDeviceReport,
    game_input_device_v0_GetRawDeviceFeature,
    game_input_device_v0_SetRawDeviceFeature,
    game_input_device_v0_SendRawDeviceOutput,
    game_input_device_v0_SendRawDeviceOutputWithResponse,
    game_input_device_v0_ExecuteRawDeviceIoControl,
    game_input_device_v0_AcquireExclusiveRawDeviceAccess,
    game_input_device_v0_ReleaseExclusiveRawDeviceAccess,
};

static HRESULT WINAPI game_input_device_v2_QueryInterface( IGameInputDevice_v2 *iface, REFIID iid, void **out )
{
    struct device *device = device_from_IGameInputDevice_v2( iface );

    TRACE( "device %p, iid %s, out %p.\n", device, debugstr_guid( iid ), out );
    return device_query_interface( device, iid, out );
}

static ULONG WINAPI game_input_device_v2_AddRef( IGameInputDevice_v2 *iface )
{
    return device_addref( device_from_IGameInputDevice_v2( iface ) );
}

static ULONG WINAPI game_input_device_v2_Release( IGameInputDevice_v2 *iface )
{
    return device_release( device_from_IGameInputDevice_v2( iface ) );
}

static HRESULT WINAPI game_input_device_v2_GetDeviceInfo( IGameInputDevice_v2 *iface,
                                                           const GameInputDeviceInfo_v2 **info )
{
    struct device *device = device_from_IGameInputDevice_v2( iface );

    if (!info) return E_POINTER;
    *info = &device->info_v2;
    return S_OK;
}

static HRESULT WINAPI game_input_device_v2_GetHapticInfo( IGameInputDevice_v2 *iface, GameInputHapticInfo *info )
{
    if (info) memset( info, 0, sizeof(*info) );
    return GAMEINPUT_E_HAPTIC_INFO_NOT_FOUND;
}

static GameInputDeviceStatus WINAPI game_input_device_v2_GetDeviceStatus( IGameInputDevice_v2 *iface )
{
    return device_from_IGameInputDevice_v2( iface )->status;
}

static HRESULT WINAPI game_input_device_v2_CreateForceFeedbackEffect( IGameInputDevice_v2 *iface, uint32_t index,
                                                                       const GameInputForceFeedbackParams *params,
                                                                       IGameInputForceFeedbackEffect_v2 **effect )
{
    if (effect) *effect = NULL;
    return GAMEINPUT_E_FEEDBACK_NOT_SUPPORTED;
}

static bool WINAPI game_input_device_v2_IsForceFeedbackMotorPoweredOn( IGameInputDevice_v2 *iface, uint32_t index )
{
    return FALSE;
}

static void WINAPI game_input_device_v2_SetForceFeedbackMotorGain( IGameInputDevice_v2 *iface, uint32_t index, float gain )
{
}

static void WINAPI game_input_device_v2_SetRumbleState( IGameInputDevice_v2 *iface, const GameInputRumbleParams *params )
{
}

static HRESULT WINAPI game_input_device_v2_DirectInputEscape( IGameInputDevice_v2 *iface, uint32_t command,
                                                               const void *input, uint32_t input_size,
                                                               void *output, uint32_t output_size, uint32_t *output_count )
{
    if (output_count) *output_count = 0;
    return E_NOTIMPL;
}

static const IGameInputDevice_v2Vtbl game_input_device_v2_vtbl =
{
    game_input_device_v2_QueryInterface,
    game_input_device_v2_AddRef,
    game_input_device_v2_Release,
    game_input_device_v2_GetDeviceInfo,
    game_input_device_v2_GetHapticInfo,
    game_input_device_v2_GetDeviceStatus,
    game_input_device_v2_CreateForceFeedbackEffect,
    game_input_device_v2_IsForceFeedbackMotorPoweredOn,
    game_input_device_v2_SetForceFeedbackMotorGain,
    game_input_device_v2_SetRumbleState,
    game_input_device_v2_DirectInputEscape,
};

static BOOL matches_device_interface( const DEV_OBJECT *object, const GUID *iid )
{
    for (UINT i = 0; i < object->cPropertyCount; i++)
    {
        const DEVPROPERTY *prop = object->pProperties + i;
        if (memcmp( &DEVPKEY_DeviceInterface_ClassGuid, &prop->CompKey.Key, sizeof(prop->CompKey.Key) )) continue;
        return IsEqualGUID( prop->Buffer, iid );
    }

    return FALSE;
}

static struct device *device_create( struct list *devices, const DEV_OBJECT *object )
{
    GameInputDeviceFamily family = GameInputFamilyHid;
    const WCHAR *device_path = object->pszObjectId;
    PHIDP_PREPARSED_DATA preparsed;
    struct device *device = NULL;
    HIDD_ATTRIBUTES attr;
    HIDP_CAPS caps;
    HANDLE file;
    WCHAR *tmp;

    if (!matches_device_interface( object, &GUID_DEVINTERFACE_HID ) &&
        !matches_device_interface( object, &GUID_DEVINTERFACE_WINEXINPUT ))
        return NULL;

    if ((tmp = wcschr( device_path + 8, '#' )) && !wcsnicmp( tmp - 6, L"&IG_", 4 )) return NULL;
    if (tmp && !wcsnicmp( tmp - 6, L"&XI_", 4 )) family = GameInputFamilyXbox360;

    TRACE( "device_path %s\n", debugstr_w( device_path ) );

    file = CreateFileW(device_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;

    HidD_GetAttributes( file, &attr );
    HidD_GetPreparsedData( file, &preparsed );
    HidP_GetCaps( preparsed, &caps );
    HidD_FreePreparsedData( preparsed );

    if (caps.UsagePage != HID_USAGE_PAGE_GENERIC || caps.Usage != HID_USAGE_GENERIC_GAMEPAD) goto done;
    /* CW-Bug-Id: #23185 Emulate Steam Input native hooks for native SDL */
    if (attr.VendorID == 0x28de && attr.ProductID == 0x11ff) goto done;

    if (!(device = calloc( 1, sizeof(*device) ))) goto done;
    device->IGameInputDevice_v0_iface.lpVtbl = &game_input_device_v0_vtbl;
    device->IGameInputDevice_v2_iface.lpVtbl = &game_input_device_v2_vtbl;
    device->refcount = 1;
    wcscpy( device->path, device_path );

    device->info_v0.infoSize = sizeof(device->info_v0.infoSize);
    device->info_v0.vendorId = attr.VendorID;
    device->info_v0.productId = attr.ProductID;
    device->info_v0.usage.page = caps.UsagePage;
    device->info_v0.usage.id = caps.Usage;
    device->info_v0.deviceFamily = family;
    device->info_v0.capabilities = GameInputDeviceCapabilityNone;
    device->info_v0.supportedInput = GameInputKindUiNavigation_v0 | GameInputKindGamepad | GameInputKindController;
    device->info_v0.supportedRumbleMotors = GameInputRumbleNone;
    device->info_v0.controllerAxisCount = 6;
    device->info_v0.controllerButtonCount = caps.NumberInputButtonCaps;
    device->info_v0.controllerSwitchCount = 1;
    device->info_v0.supportedSystemButtons = GameInputSystemButtonGuide;

    device->info_v2.vendorId = attr.VendorID;
    device->info_v2.productId = attr.ProductID;
    device->info_v2.usage.page = caps.UsagePage;
    device->info_v2.usage.id = caps.Usage;
    device->info_v2.deviceFamily = family;
    device->info_v2.supportedInput = GameInputKindGamepad | GameInputKindController;
    device->info_v2.supportedSystemButtons = GameInputSystemButtonGuide;
    device->info_v2.controllerAxisCount = 6;
    device->info_v2.controllerButtonCount = caps.NumberInputButtonCaps;
    device->info_v2.controllerSwitchCount = 1;

    list_add_tail( devices, &device->entry );

    TRACE( "created device %p\n", device );
done:
    CloseHandle( file );
    return device;
}

static struct device *mouse_device_create( struct list *devices )
{
    struct device *device;

    if (!(device = calloc( 1, sizeof(*device) ))) return NULL;
    device->IGameInputDevice_v0_iface.lpVtbl = &game_input_device_v0_vtbl;
    device->IGameInputDevice_v2_iface.lpVtbl = &game_input_device_v2_vtbl;
    device->refcount = 1;
    wcscpy( device->path, L"Wine virtual mouse" );
    device->status = GameInputDeviceConnected | GameInputDeviceInputEnabled;

    device->mouse_info_v0.supportedButtons = GameInputMouseLeftButton |
            GameInputMouseRightButton | GameInputMouseMiddleButton |
            GameInputMouseButton4 | GameInputMouseButton5;
    device->mouse_info_v2.supportedButtons = device->mouse_info_v0.supportedButtons;

    device->info_v0.infoSize = sizeof(device->info_v0);
    device->info_v0.usage.page = HID_USAGE_PAGE_GENERIC;
    device->info_v0.usage.id = HID_USAGE_GENERIC_MOUSE;
    device->info_v0.deviceFamily = GameInputFamilyVirtual;
    device->info_v0.supportedInput = GameInputKindMouse;
    device->info_v0.mouseInfo = &device->mouse_info_v0;
    memcpy( device->info_v0.deviceId.value, "WINE_MOUSE", sizeof("WINE_MOUSE") );
    device->info_v0.deviceRootId = device->info_v0.deviceId;

    device->info_v2.usage = device->info_v0.usage;
    device->info_v2.deviceId = device->info_v0.deviceId;
    device->info_v2.deviceRootId = device->info_v0.deviceRootId;
    device->info_v2.deviceFamily = GameInputFamilyVirtual;
    device->info_v2.supportedInput = GameInputKindMouse;
    device->info_v2.displayName = "Wine virtual mouse";
    device->info_v2.pnpPath = "Wine virtual mouse";
    device->info_v2.mouseInfo = &device->mouse_info_v2;

    list_add_tail( devices, &device->entry );
    return device;
}

struct mouse_reading
{
    IGameInputReading_v0 IGameInputReading_v0_iface;
    IGameInputReading_v2 IGameInputReading_v2_iface;
    LONG refcount;
    struct device *device;
    GameInputMouseState_v0 state_v0;
    GameInputMouseState_v2 state_v2;
    uint64_t timestamp;
    uint64_t sequence;
};

static struct mouse_reading *mouse_reading_from_iface( IGameInputReading_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct mouse_reading, IGameInputReading_v0_iface );
}

static struct mouse_reading *mouse_reading_from_v2_iface( IGameInputReading_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct mouse_reading, IGameInputReading_v2_iface );
}

static ULONG mouse_reading_addref( struct mouse_reading *reading )
{
    return InterlockedIncrement( &reading->refcount );
}

static ULONG mouse_reading_release( struct mouse_reading *reading )
{
    ULONG ref = InterlockedDecrement( &reading->refcount );

    if (!ref)
    {
        device_release( reading->device );
        free( reading );
    }
    return ref;
}

static HRESULT mouse_reading_query_interface( struct mouse_reading *reading, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IGameInputReading_v0 ))
        *out = &reading->IGameInputReading_v0_iface;
    else if (IsEqualGUID( iid, &IID_IGameInputReading_v2 ))
        *out = &reading->IGameInputReading_v2_iface;
    else
        return E_NOINTERFACE;

    mouse_reading_addref( reading );
    return S_OK;
}

static HRESULT WINAPI mouse_reading_QueryInterface( IGameInputReading_v0 *iface, REFIID iid, void **out )
{
    return mouse_reading_query_interface( mouse_reading_from_iface( iface ), iid, out );
}

static ULONG WINAPI mouse_reading_AddRef( IGameInputReading_v0 *iface )
{
    return mouse_reading_addref( mouse_reading_from_iface( iface ) );
}

static ULONG WINAPI mouse_reading_Release( IGameInputReading_v0 *iface )
{
    return mouse_reading_release( mouse_reading_from_iface( iface ) );
}

static GameInputKind WINAPI mouse_reading_GetInputKind( IGameInputReading_v0 *iface )
{
    return GameInputKindMouse;
}

static uint64_t WINAPI mouse_reading_GetSequenceNumber( IGameInputReading_v0 *iface, GameInputKind kind )
{
    struct mouse_reading *reading = mouse_reading_from_iface( iface );
    return kind & GameInputKindMouse ? reading->sequence : 0;
}

static uint64_t WINAPI mouse_reading_GetTimestamp( IGameInputReading_v0 *iface )
{
    return mouse_reading_from_iface( iface )->timestamp;
}

static void WINAPI mouse_reading_GetDevice( IGameInputReading_v0 *iface, IGameInputDevice_v0 **device )
{
    struct mouse_reading *reading = mouse_reading_from_iface( iface );

    if (!device) return;
    device_addref( reading->device );
    *device = &reading->device->IGameInputDevice_v0_iface;
}

static bool WINAPI mouse_reading_GetRawReport( IGameInputReading_v0 *iface, IGameInputRawDeviceReport_v0 **report )
{
    if (report) *report = NULL;
    return FALSE;
}

static uint32_t WINAPI mouse_reading_GetControllerAxisCount( IGameInputReading_v0 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetControllerAxisState( IGameInputReading_v0 *iface, uint32_t count, float *state )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetControllerButtonCount( IGameInputReading_v0 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetControllerButtonState( IGameInputReading_v0 *iface, uint32_t count, bool *state )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetControllerSwitchCount( IGameInputReading_v0 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetControllerSwitchState( IGameInputReading_v0 *iface, uint32_t count, GameInputSwitchPosition *state )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetKeyCount( IGameInputReading_v0 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetKeyState( IGameInputReading_v0 *iface, uint32_t count, GameInputKeyState *state )
{
    return 0;
}

static bool WINAPI mouse_reading_GetMouseState( IGameInputReading_v0 *iface, GameInputMouseState_v0 *state )
{
    if (!state) return FALSE;
    *state = mouse_reading_from_iface( iface )->state_v0;
    return TRUE;
}

static uint32_t WINAPI mouse_reading_GetTouchCount( IGameInputReading_v0 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_GetTouchState( IGameInputReading_v0 *iface, uint32_t count, GameInputTouchState *state )
{
    return 0;
}

static bool WINAPI mouse_reading_GetMotionState( IGameInputReading_v0 *iface, GameInputMotionState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_GetArcadeStickState( IGameInputReading_v0 *iface, GameInputArcadeStickState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_GetFlightStickState( IGameInputReading_v0 *iface, GameInputFlightStickState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_GetGamepadState( IGameInputReading_v0 *iface, GameInputGamepadState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_GetRacingWheelState( IGameInputReading_v0 *iface, GameInputRacingWheelState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_GetUiNavigationState( IGameInputReading_v0 *iface, GameInputUiNavigationState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static const IGameInputReading_v0Vtbl mouse_reading_vtbl =
{
    mouse_reading_QueryInterface,
    mouse_reading_AddRef,
    mouse_reading_Release,
    mouse_reading_GetInputKind,
    mouse_reading_GetSequenceNumber,
    mouse_reading_GetTimestamp,
    mouse_reading_GetDevice,
    mouse_reading_GetRawReport,
    mouse_reading_GetControllerAxisCount,
    mouse_reading_GetControllerAxisState,
    mouse_reading_GetControllerButtonCount,
    mouse_reading_GetControllerButtonState,
    mouse_reading_GetControllerSwitchCount,
    mouse_reading_GetControllerSwitchState,
    mouse_reading_GetKeyCount,
    mouse_reading_GetKeyState,
    mouse_reading_GetMouseState,
    mouse_reading_GetTouchCount,
    mouse_reading_GetTouchState,
    mouse_reading_GetMotionState,
    mouse_reading_GetArcadeStickState,
    mouse_reading_GetFlightStickState,
    mouse_reading_GetGamepadState,
    mouse_reading_GetRacingWheelState,
    mouse_reading_GetUiNavigationState,
};

static HRESULT WINAPI mouse_reading_v2_QueryInterface( IGameInputReading_v2 *iface, REFIID iid, void **out )
{
    return mouse_reading_query_interface( mouse_reading_from_v2_iface( iface ), iid, out );
}

static ULONG WINAPI mouse_reading_v2_AddRef( IGameInputReading_v2 *iface )
{
    return mouse_reading_addref( mouse_reading_from_v2_iface( iface ) );
}

static ULONG WINAPI mouse_reading_v2_Release( IGameInputReading_v2 *iface )
{
    return mouse_reading_release( mouse_reading_from_v2_iface( iface ) );
}

static GameInputKind WINAPI mouse_reading_v2_GetInputKind( IGameInputReading_v2 *iface )
{
    return GameInputKindMouse;
}

static uint64_t WINAPI mouse_reading_v2_GetTimestamp( IGameInputReading_v2 *iface )
{
    return mouse_reading_from_v2_iface( iface )->timestamp;
}

static void WINAPI mouse_reading_v2_GetDevice( IGameInputReading_v2 *iface, IGameInputDevice_v2 **device )
{
    struct mouse_reading *reading = mouse_reading_from_v2_iface( iface );

    if (!device) return;
    device_addref( reading->device );
    *device = &reading->device->IGameInputDevice_v2_iface;
}

static uint32_t WINAPI mouse_reading_v2_GetControllerAxisCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetControllerAxisState( IGameInputReading_v2 *iface, uint32_t count, float *state )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetControllerButtonCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetControllerButtonState( IGameInputReading_v2 *iface, uint32_t count, bool *state )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetControllerSwitchCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetControllerSwitchState( IGameInputReading_v2 *iface, uint32_t count,
                                                                  GameInputSwitchPosition *state )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetKeyCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI mouse_reading_v2_GetKeyState( IGameInputReading_v2 *iface, uint32_t count, GameInputKeyState *state )
{
    return 0;
}

static bool WINAPI mouse_reading_v2_GetMouseState( IGameInputReading_v2 *iface, GameInputMouseState_v2 *state )
{
    if (!state) return FALSE;
    *state = mouse_reading_from_v2_iface( iface )->state_v2;
    return TRUE;
}

static bool WINAPI mouse_reading_v2_GetSensorsState( IGameInputReading_v2 *iface, GameInputSensorsState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_v2_GetArcadeStickState( IGameInputReading_v2 *iface, GameInputArcadeStickState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_v2_GetFlightStickState( IGameInputReading_v2 *iface, GameInputFlightStickState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_v2_GetGamepadState( IGameInputReading_v2 *iface, GameInputGamepadState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_v2_GetRacingWheelState( IGameInputReading_v2 *iface, GameInputRacingWheelState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static bool WINAPI mouse_reading_v2_GetUiNavigationState( IGameInputReading_v2 *iface, GameInputUiNavigationState *state )
{
    if (state) memset( state, 0, sizeof(*state) );
    return FALSE;
}

static const IGameInputReading_v2Vtbl mouse_reading_v2_vtbl =
{
    mouse_reading_v2_QueryInterface,
    mouse_reading_v2_AddRef,
    mouse_reading_v2_Release,
    mouse_reading_v2_GetInputKind,
    mouse_reading_v2_GetTimestamp,
    mouse_reading_v2_GetDevice,
    mouse_reading_v2_GetControllerAxisCount,
    mouse_reading_v2_GetControllerAxisState,
    mouse_reading_v2_GetControllerButtonCount,
    mouse_reading_v2_GetControllerButtonState,
    mouse_reading_v2_GetControllerSwitchCount,
    mouse_reading_v2_GetControllerSwitchState,
    mouse_reading_v2_GetKeyCount,
    mouse_reading_v2_GetKeyState,
    mouse_reading_v2_GetMouseState,
    mouse_reading_v2_GetSensorsState,
    mouse_reading_v2_GetArcadeStickState,
    mouse_reading_v2_GetFlightStickState,
    mouse_reading_v2_GetGamepadState,
    mouse_reading_v2_GetRacingWheelState,
    mouse_reading_v2_GetUiNavigationState,
};

static HRESULT mouse_reading_create( struct device *device, const GameInputMouseState_v2 *state,
                                     uint64_t timestamp, uint64_t sequence, struct mouse_reading **out )
{
    struct mouse_reading *reading;

    if (!(reading = calloc( 1, sizeof(*reading) ))) return E_OUTOFMEMORY;
    reading->IGameInputReading_v0_iface.lpVtbl = &mouse_reading_vtbl;
    reading->IGameInputReading_v2_iface.lpVtbl = &mouse_reading_v2_vtbl;
    reading->refcount = 1;
    device_addref( reading->device = device );
    reading->state_v2 = *state;
    reading->state_v0.buttons = state->buttons;
    reading->state_v0.positionX = state->positionX;
    reading->state_v0.positionY = state->positionY;
    reading->state_v0.wheelX = state->wheelX;
    reading->state_v0.wheelY = state->wheelY;
    reading->timestamp = timestamp;
    reading->sequence = sequence;
    *out = reading;
    return S_OK;
}

struct device_callback
{
    struct list entry;

    IGameInputDevice_v0 *device;
    GameInputKind input_kind;
    GameInputDeviceStatus status_filter;

    void *context;
    GameInputDeviceCallback_v0 callback;
};

struct device_callback_v2
{
    struct list entry;
    LONG refcount;

    IGameInputDevice_v2 *device;
    GameInputKind input_kind;
    GameInputDeviceStatus status_filter;

    void *context;
    GameInputDeviceCallback_v2 callback;
    HANDLE idle_event;
    CRITICAL_SECTION callback_cs;
    LONG executing;
    BOOL stopped;
};

struct device_callback_v2_snapshot
{
    struct device *device;
    GameInputKind input_kind;
    GameInputDeviceStatus current_status;
    GameInputDeviceStatus previous_status;
};

struct device_callback_v2_job
{
    struct device_callback_v2 *callback;
    SIZE_T count;
    struct device_callback_v2_snapshot snapshots[1];
};

struct device_callbacks_v2_dispatch
{
    struct device *device;
    GameInputKind input_kind;
    GameInputDeviceStatus current_status;
    GameInputDeviceStatus previous_status;
    SIZE_T count;
    struct device_callback_v2 *callbacks[1];
};

static void device_callback_notify( struct device_callback *entry, IGameInputDevice_v0 *device, GameInputKind input_kind,
                                    GameInputDeviceStatus new_status, GameInputDeviceStatus old_status )
{
    if (entry->device && entry->device != device) return;
    if (!(entry->input_kind & input_kind)) return;
    if (!(entry->status_filter & (new_status | old_status))) return;
    entry->callback( (UINT_PTR)entry, entry->context, device, 0, new_status, old_status );
}

static HRESULT device_callback_create( IGameInputDevice_v0 *device, GameInputKind input_kind, GameInputDeviceStatus status_filter,
                                       void *context, GameInputDeviceCallback_v0 callback, struct device_callback **out )
{
    struct device_callback *entry;

    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    if ((entry->device = device)) IGameInputDevice_v0_AddRef( entry->device );
    entry->input_kind = input_kind;
    entry->status_filter = status_filter;
    entry->context = context;
    entry->callback = callback;

    *out = entry;
    return S_OK;
}

static ULONG device_callback_v2_addref( struct device_callback_v2 *entry )
{
    return InterlockedIncrement( &entry->refcount );
}

static ULONG device_callback_v2_release( struct device_callback_v2 *entry )
{
    ULONG ref = InterlockedDecrement( &entry->refcount );

    if (!ref)
    {
        if (entry->device) IGameInputDevice_v2_Release( entry->device );
        DeleteCriticalSection( &entry->callback_cs );
        CloseHandle( entry->idle_event );
        free( entry );
    }
    return ref;
}

static HRESULT device_callback_v2_create( IGameInputDevice_v2 *device, GameInputKind input_kind,
                                          GameInputDeviceStatus status_filter, void *context,
                                          GameInputDeviceCallback_v2 callback,
                                          struct device_callback_v2 **out )
{
    struct device_callback_v2 *entry = NULL;
    HRESULT hr;

    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    if (!(entry->idle_event = CreateEventW( NULL, TRUE, TRUE, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        free( entry );
        return hr;
    }

    entry->refcount = 1;
    InitializeCriticalSection( &entry->callback_cs );
    if ((entry->device = device)) IGameInputDevice_v2_AddRef( entry->device );
    entry->input_kind = input_kind;
    entry->status_filter = status_filter;
    entry->context = context;
    entry->callback = callback;
    *out = entry;
    return S_OK;
}

struct game_input
{
    IGameInput_v0 IGameInput_v0_iface;
    IGameInput_v2 IGameInput_v2_iface;
    LONG refcount;

    HDEVQUERY query;
    HANDLE initialized;
    struct list devices;
    struct list callbacks;
    struct list callbacks_v2;
    struct device *mouse_device;
    struct mouse_reading *current_mouse_reading;
    POINT last_cursor;
    BOOL cursor_initialized;
    BOOL mouse_foreground;
    int64_t mouse_position_x;
    int64_t mouse_position_y;
    uint64_t mouse_sequence;
};

static struct game_input *impl_from_v0_IGameInput( IGameInput_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct game_input, IGameInput_v0_iface );
}

static struct game_input *impl_from_v2_IGameInput( IGameInput_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct game_input, IGameInput_v2_iface );
}

static ULONG game_input_addref( struct game_input *impl )
{
    ULONG ref;

    EnterCriticalSection( &game_input_cs );
    ref = ++impl->refcount;
    LeaveCriticalSection( &game_input_cs );
    TRACE( "impl %p increasing refcount to %lu.\n", impl, ref );
    return ref;
}

static ULONG game_input_release( struct game_input *impl )
{
    ULONG ref;

    EnterCriticalSection( &game_input_cs );
    if (!(ref = --impl->refcount)) game_input = NULL;
    LeaveCriticalSection( &game_input_cs );
    TRACE( "impl %p decreasing refcount to %lu.\n", impl, ref );

    if (!ref)
    {
        struct list *ptr;

        if (impl->current_mouse_reading) mouse_reading_release( impl->current_mouse_reading );

        while ((ptr = list_head( &impl->callbacks )))
        {
            struct device_callback *callback = LIST_ENTRY(ptr, struct device_callback, entry);
            list_remove( &callback->entry );
            if (callback->device) IGameInputDevice_v0_Release( callback->device );
            free( callback );
        }

        for (;;)
        {
            struct device_callback_v2 *callback;

            EnterCriticalSection( &game_input_cs );
            if (!(ptr = list_head( &impl->callbacks_v2 )))
            {
                LeaveCriticalSection( &game_input_cs );
                break;
            }
            callback = LIST_ENTRY(ptr, struct device_callback_v2, entry);
            list_remove( &callback->entry );
            callback->stopped = TRUE;
            LeaveCriticalSection( &game_input_cs );

            WaitForSingleObject( callback->idle_event, INFINITE );
            device_callback_v2_release( callback );
        }

        while ((ptr = list_head( &impl->devices )))
        {
            struct device *device = LIST_ENTRY(ptr, struct device, entry);
            list_remove( &device->entry );
            device_release( device );
        }

        if (impl->query) DevCloseObjectQuery( impl->query );
        if (impl->initialized) CloseHandle( impl->initialized );
        free( impl );
    }
    return ref;
}

static HRESULT game_input_query_interface( struct game_input *impl, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IGameInput_v0 ))
        *out = &impl->IGameInput_v0_iface;
    else if (IsEqualGUID( iid, &IID_IGameInput_v2 ))
        *out = &impl->IGameInput_v2_iface;
    else
        return E_NOINTERFACE;

    game_input_addref( impl );
    return S_OK;
}

static HRESULT WINAPI game_input_v0_QueryInterface( IGameInput_v0 *iface, REFIID iid, void **out )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );

    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    return game_input_query_interface( impl, iid, out );
}

static ULONG WINAPI game_input_v0_AddRef( IGameInput_v0 *iface )
{
    return game_input_addref( impl_from_v0_IGameInput( iface ) );
}

static ULONG WINAPI game_input_v0_Release( IGameInput_v0 *iface )
{
    return game_input_release( impl_from_v0_IGameInput( iface ) );
}

static uint64_t game_input_timestamp(void)
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceCounter( &counter );
    QueryPerformanceFrequency( &frequency );
    return counter.QuadPart / frequency.QuadPart * 1000000 +
           counter.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart;
}

static void device_callback_v2_notify( struct device_callback_v2 *entry, struct device *device,
                                       GameInputKind input_kind, GameInputDeviceStatus current_status,
                                       GameInputDeviceStatus previous_status )
{
    GameInputDeviceCallback_v2 callback;
    void *context;

    if (entry->device && entry->device != &device->IGameInputDevice_v2_iface) return;
    if (!(entry->input_kind & input_kind)) return;
    if (!(entry->status_filter & (current_status ^ previous_status))) return;

    EnterCriticalSection( &entry->callback_cs );
    EnterCriticalSection( &game_input_cs );
    if (entry->stopped)
    {
        LeaveCriticalSection( &game_input_cs );
        LeaveCriticalSection( &entry->callback_cs );
        return;
    }
    if (!entry->executing++) ResetEvent( entry->idle_event );
    callback = entry->callback;
    context = entry->context;
    LeaveCriticalSection( &game_input_cs );

    callback( (UINT_PTR)entry, context, &device->IGameInputDevice_v2_iface, game_input_timestamp(),
              current_status, previous_status );

    EnterCriticalSection( &game_input_cs );
    if (!--entry->executing) SetEvent( entry->idle_event );
    LeaveCriticalSection( &game_input_cs );
    LeaveCriticalSection( &entry->callback_cs );
}

static struct device_callback_v2_job *device_callback_v2_job_create_locked(
        struct game_input *impl, struct device_callback_v2 *callback )
{
    struct device_callback_v2_job *job;
    struct device *device;
    SIZE_T count = 0, i = 0;

    LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry ) count++;
    if (!(job = calloc( 1, offsetof(struct device_callback_v2_job, snapshots) +
                            count * sizeof(*job->snapshots) )))
        return NULL;

    device_callback_v2_addref( job->callback = callback );
    job->count = count;
    LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry )
    {
        device_addref( job->snapshots[i].device = device );
        job->snapshots[i].input_kind = device->info_v2.supportedInput;
        job->snapshots[i].current_status = device->status;
        job->snapshots[i].previous_status = GameInputDeviceNoStatus;
        i++;
    }
    return job;
}

static void device_callback_v2_job_release( struct device_callback_v2_job *job )
{
    SIZE_T i;

    for (i = 0; i < job->count; ++i) device_release( job->snapshots[i].device );
    device_callback_v2_release( job->callback );
    free( job );
}

static void device_callback_v2_job_run( struct device_callback_v2_job *job )
{
    SIZE_T i;

    for (i = 0; i < job->count; ++i)
    {
        struct device_callback_v2_snapshot *snapshot = &job->snapshots[i];

        device_callback_v2_notify( job->callback, snapshot->device, snapshot->input_kind,
                                   snapshot->current_status, snapshot->previous_status );
    }
}

static DWORD CALLBACK device_callback_v2_worker( void *context )
{
    struct device_callback_v2_job *job = context;

    device_callback_v2_job_run( job );
    device_callback_v2_job_release( job );
    return 0;
}

static struct device_callbacks_v2_dispatch *device_callbacks_v2_dispatch_create_locked(
        struct game_input *impl, struct device *device, GameInputDeviceStatus previous_status )
{
    struct device_callbacks_v2_dispatch *dispatch;
    struct device_callback_v2 *callback;
    SIZE_T count = 0, i = 0;

    LIST_FOR_EACH_ENTRY( callback, &impl->callbacks_v2, struct device_callback_v2, entry ) count++;
    if (!count) return NULL;
    if (!(dispatch = calloc( 1, offsetof(struct device_callbacks_v2_dispatch, callbacks) +
                                 count * sizeof(*dispatch->callbacks) )))
        return NULL;

    device_addref( dispatch->device = device );
    dispatch->input_kind = device->info_v2.supportedInput;
    dispatch->current_status = device->status;
    dispatch->previous_status = previous_status;
    dispatch->count = count;
    LIST_FOR_EACH_ENTRY( callback, &impl->callbacks_v2, struct device_callback_v2, entry )
    {
        device_callback_v2_addref( dispatch->callbacks[i++] = callback );
    }
    return dispatch;
}

static void device_callbacks_v2_dispatch_run( struct device_callbacks_v2_dispatch *dispatch )
{
    SIZE_T i;

    if (!dispatch) return;
    for (i = 0; i < dispatch->count; ++i)
    {
        device_callback_v2_notify( dispatch->callbacks[i], dispatch->device, dispatch->input_kind,
                                   dispatch->current_status, dispatch->previous_status );
        device_callback_v2_release( dispatch->callbacks[i] );
    }
    device_release( dispatch->device );
    free( dispatch );
}

static uint64_t WINAPI game_input_v0_GetCurrentTimestamp( IGameInput_v0 *iface )
{
    return game_input_timestamp();
}

static BOOL process_has_input_focus(void)
{
    DWORD process_id = 0;
    HWND hwnd = GetForegroundWindow();

    if (hwnd) GetWindowThreadProcessId( hwnd, &process_id );
    return process_id == GetCurrentProcessId();
}

static GameInputMouseButtons get_mouse_buttons(void)
{
    GameInputMouseButtons buttons = GameInputMouseNone;

    if (GetAsyncKeyState( VK_LBUTTON ) & 0x8000) buttons |= GameInputMouseLeftButton;
    if (GetAsyncKeyState( VK_RBUTTON ) & 0x8000) buttons |= GameInputMouseRightButton;
    if (GetAsyncKeyState( VK_MBUTTON ) & 0x8000) buttons |= GameInputMouseMiddleButton;
    if (GetAsyncKeyState( VK_XBUTTON1 ) & 0x8000) buttons |= GameInputMouseButton4;
    if (GetAsyncKeyState( VK_XBUTTON2 ) & 0x8000) buttons |= GameInputMouseButton5;
    return buttons;
}

static HRESULT game_input_get_current_mouse_reading( struct game_input *impl, struct mouse_reading **reading )
{
    GameInputMouseState_v2 state = {0};
    struct mouse_reading *new_reading;
    BOOL foreground;
    POINT cursor;
    HRESULT hr;

    *reading = NULL;
    EnterCriticalSection( &game_input_cs );
    foreground = process_has_input_focus();
    if (GetCursorPos( &cursor ))
    {
        if (foreground && impl->cursor_initialized && impl->mouse_foreground)
        {
            impl->mouse_position_x += cursor.x - impl->last_cursor.x;
            impl->mouse_position_y += cursor.y - impl->last_cursor.y;
        }
        impl->last_cursor = cursor;
        impl->cursor_initialized = TRUE;
    }
    impl->mouse_foreground = foreground;

    state.positions = GameInputMouseRelativePosition;
    state.positionX = impl->mouse_position_x;
    state.positionY = impl->mouse_position_y;
    if (impl->cursor_initialized)
    {
        state.positions |= GameInputMouseAbsolutePosition;
        state.absolutePositionX = impl->last_cursor.x;
        state.absolutePositionY = impl->last_cursor.y;
    }
    if (foreground) state.buttons = get_mouse_buttons();

    if (impl->current_mouse_reading)
    {
        if (!memcmp( &impl->current_mouse_reading->state_v2, &state, sizeof(state) ))
        {
            mouse_reading_addref( *reading = impl->current_mouse_reading );
            LeaveCriticalSection( &game_input_cs );
            return S_OK;
        }
    }

    hr = mouse_reading_create( impl->mouse_device, &state, game_input_timestamp(), ++impl->mouse_sequence,
                               &new_reading );
    if (SUCCEEDED(hr))
    {
        if (impl->current_mouse_reading) mouse_reading_release( impl->current_mouse_reading );
        mouse_reading_addref( impl->current_mouse_reading = new_reading );
        *reading = new_reading;
    }
    LeaveCriticalSection( &game_input_cs );
    return hr;
}

static HRESULT WINAPI game_input_v0_GetCurrentReading( IGameInput_v0 *iface, GameInputKind input_kind,
                                                       IGameInputDevice_v0 *device_iface, IGameInputReading_v0 **reading )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );
    struct mouse_reading *mouse_reading;
    HRESULT hr;

    TRACE( "impl %p, input_kind %#x, device %p, reading %p.\n", impl, input_kind, device_iface, reading );
    if (!reading) return E_POINTER;
    *reading = NULL;
    if (!(input_kind & GameInputKindMouse)) return GAMEINPUT_E_READING_NOT_FOUND;
    if (device_iface && device_iface != &impl->mouse_device->IGameInputDevice_v0_iface)
        return GAMEINPUT_E_READING_NOT_FOUND;

    if (SUCCEEDED(hr = game_input_get_current_mouse_reading( impl, &mouse_reading )))
        *reading = &mouse_reading->IGameInputReading_v0_iface;
    return hr;
}

static HRESULT WINAPI game_input_v0_GetNextReading( IGameInput_v0 *iface, IGameInputReading_v0 *reference_reading, GameInputKind input_kind,
                                                    IGameInputDevice_v0 *device_iface, IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, reference_reading %p, input_kind %#x, device %p, reading %p\n", impl_from_v0_IGameInput( iface ), reference_reading, input_kind, device_iface, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_GetPreviousReading( IGameInput_v0 *iface, IGameInputReading_v0 *reference_reading,
                                                        GameInputKind input_kind, IGameInputDevice_v0 *device,
                                                        IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, reference_reading %p, input_kind %#x, device %p, reading %p stub!\n",
           impl_from_v0_IGameInput( iface ), reference_reading, input_kind, device, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_GetTemporalReading( IGameInput_v0 *iface, uint64_t timestamp, IGameInputDevice_v0 *device, IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, timestamp %I64u, device %p, reading %p stub!\n", impl_from_v0_IGameInput( iface ), timestamp, device, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_RegisterReadingCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device, GameInputKind input_kind, float analog_threshold,
                                                             void *context, GameInputReadingCallback_v0 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, input_kind %#x, analog_threshold %f, context %p, callback %p, token %p stub!\n",
           impl_from_v0_IGameInput( iface ), device, input_kind, analog_threshold, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_RegisterDeviceCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device_iface, GameInputKind input_kind, GameInputDeviceStatus status_filter,
                                                            GameInputEnumerationKind enumeration_kind, void *context, GameInputDeviceCallback_v0 callback,
                                                            GameInputCallbackToken *token )
{
    struct game_input *impl = CONTAINING_RECORD( iface, struct game_input, IGameInput_v0_iface );
    struct device_callback *entry;
    struct device *device;
    HRESULT hr;

    FIXME( "impl %p device_iface %p input_kind %#x status_filter %#x enumeration_kind %#x, context %p callback %p token %p stub!\n",
           impl, device_iface, input_kind, status_filter, enumeration_kind, context, callback, token );

    if (FAILED(hr = device_callback_create( device_iface, input_kind, status_filter, context, callback, &entry ))) return hr;

    EnterCriticalSection( &game_input_cs );
    list_add_tail( &impl->callbacks, &entry->entry );

    LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry )
        device_callback_notify( entry, &device->IGameInputDevice_v0_iface, GameInputKindGamepad,
                                device->status, GameInputDeviceNoStatus );

    LeaveCriticalSection( &game_input_cs );

    *token = (UINT_PTR)entry;

    return hr;
}

static HRESULT WINAPI game_input_v0_RegisterSystemButtonCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device,
                                                                  GameInputSystemButtons button_filter, void *context,
                                                                  GameInputSystemButtonCallback_v0 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, button_filter %#x, context %p, callback %p, token %p stub!\n",
           impl_from_v0_IGameInput( iface ), device, button_filter, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_RegisterKeyboardLayoutCallback( IGameInput_v0 *iface, IGameInputDevice_v0 *device, void *context,
                                                                    GameInputKeyboardLayoutCallback_v0 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, context %p, callback %p, token %p stub!\n",
           impl_from_v0_IGameInput( iface ), device, context, callback, token );
    return E_NOTIMPL;
}

static void WINAPI game_input_v0_StopCallback( IGameInput_v0 *iface, GameInputCallbackToken token )
{
    FIXME( "impl %p, token %I64u stub!\n", impl_from_v0_IGameInput( iface ), token );
}

static bool WINAPI game_input_v0_UnregisterCallback( IGameInput_v0 *iface, GameInputCallbackToken token, uint64_t timeout_us )
{
    FIXME( "impl %p, token %I64u, timeout_us %I64u stub!\n", impl_from_v0_IGameInput( iface ), token, timeout_us );
    return FALSE;
}

static HRESULT WINAPI game_input_v0_CreateDispatcher( IGameInput_v0 *iface, IGameInputDispatcher **dispatcher )
{
    FIXME( "impl %p, dispatcher %p stub!\n", impl_from_v0_IGameInput( iface ), dispatcher );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_CreateAggregateDevice( IGameInput_v0 *iface, GameInputKind input_kind, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, input_kind %#x, device %p stub!\n", impl_from_v0_IGameInput( iface ), input_kind, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromId( IGameInput_v0 *iface, const APP_LOCAL_DEVICE_ID *value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v0_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromObject( IGameInput_v0 *iface, IUnknown *value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v0_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromPlatformHandle( IGameInput_v0 *iface, HANDLE value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v0_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_FindDeviceFromPlatformString( IGameInput_v0 *iface, const WCHAR *value, IGameInputDevice_v0 **device )
{
    FIXME( "impl %p, value %s, device %p stub!\n", impl_from_v0_IGameInput( iface ), debugstr_w(value), device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v0_EnableOemDeviceSupport( IGameInput_v0 *iface, uint16_t vendor_id, uint16_t product_id,
                                                            uint8_t interface_number, uint8_t collection_number )
{
    FIXME( "impl %p, vendor_id %u, product_id %u, interface_number %u, collection_number %u stub!\n",
           impl_from_v0_IGameInput( iface ), vendor_id, product_id, interface_number, collection_number );
    return E_NOTIMPL;
}

static void WINAPI game_input_v0_SetFocusPolicy( IGameInput_v0 *iface, GameInputFocusPolicy policy )
{
    FIXME( "impl %p, policy %#x stub!\n", impl_from_v0_IGameInput( iface ), policy );
}

static const IGameInput_v0Vtbl game_input_v0_vtbl =
{
    game_input_v0_QueryInterface,
    game_input_v0_AddRef,
    game_input_v0_Release,
    game_input_v0_GetCurrentTimestamp,
    game_input_v0_GetCurrentReading,
    game_input_v0_GetNextReading,
    game_input_v0_GetPreviousReading,
    game_input_v0_GetTemporalReading,
    game_input_v0_RegisterReadingCallback,
    game_input_v0_RegisterDeviceCallback,
    game_input_v0_RegisterSystemButtonCallback,
    game_input_v0_RegisterKeyboardLayoutCallback,
    game_input_v0_StopCallback,
    game_input_v0_UnregisterCallback,
    game_input_v0_CreateDispatcher,
    game_input_v0_CreateAggregateDevice,
    game_input_v0_FindDeviceFromId,
    game_input_v0_FindDeviceFromObject,
    game_input_v0_FindDeviceFromPlatformHandle,
    game_input_v0_FindDeviceFromPlatformString,
    game_input_v0_EnableOemDeviceSupport,
    game_input_v0_SetFocusPolicy,
};

static HRESULT WINAPI game_input_v2_QueryInterface( IGameInput_v2 *iface, REFIID iid, void **out )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );

    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );
    return game_input_query_interface( impl, iid, out );
}

static ULONG WINAPI game_input_v2_AddRef( IGameInput_v2 *iface )
{
    return game_input_addref( impl_from_v2_IGameInput( iface ) );
}

static ULONG WINAPI game_input_v2_Release( IGameInput_v2 *iface )
{
    return game_input_release( impl_from_v2_IGameInput( iface ) );
}

static uint64_t WINAPI game_input_v2_GetCurrentTimestamp( IGameInput_v2 *iface )
{
    TRACE( "impl %p.\n", impl_from_v2_IGameInput( iface ) );
    return game_input_timestamp();
}

static HRESULT WINAPI game_input_v2_GetCurrentReading( IGameInput_v2 *iface, GameInputKind input_kind,
                                                        IGameInputDevice_v2 *device_iface, IGameInputReading_v2 **reading )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct mouse_reading *mouse_reading;
    HRESULT hr;

    TRACE( "impl %p, input_kind %#x, device %p, reading %p.\n", impl, input_kind, device_iface, reading );
    if (!reading) return E_POINTER;
    *reading = NULL;
    if (!(input_kind & GameInputKindMouse)) return GAMEINPUT_E_READING_NOT_FOUND;
    if (device_iface && device_iface != &impl->mouse_device->IGameInputDevice_v2_iface)
        return GAMEINPUT_E_READING_NOT_FOUND;

    if (SUCCEEDED(hr = game_input_get_current_mouse_reading( impl, &mouse_reading )))
        *reading = &mouse_reading->IGameInputReading_v2_iface;
    return hr;
}

static HRESULT WINAPI game_input_v2_GetNextReading( IGameInput_v2 *iface, IGameInputReading_v2 *reference,
                                                     GameInputKind kind, IGameInputDevice_v2 *device,
                                                     IGameInputReading_v2 **reading )
{
    TRACE( "impl %p, reference %p, kind %#x, device %p, reading %p.\n",
           impl_from_v2_IGameInput( iface ), reference, kind, device, reading );
    if (reading) *reading = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_GetPreviousReading( IGameInput_v2 *iface, IGameInputReading_v2 *reference,
                                                         GameInputKind kind, IGameInputDevice_v2 *device,
                                                         IGameInputReading_v2 **reading )
{
    TRACE( "impl %p, reference %p, kind %#x, device %p, reading %p.\n",
           impl_from_v2_IGameInput( iface ), reference, kind, device, reading );
    if (reading) *reading = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_RegisterReadingCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device,
                                                              GameInputKind kind, void *context,
                                                              GameInputReadingCallback_v2 callback,
                                                              GameInputCallbackToken *token )
{
    TRACE( "impl %p, device %p, kind %#x, context %p, callback %p, token %p.\n",
           impl_from_v2_IGameInput( iface ), device, kind, context, callback, token );
    if (token) *token = 0;
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_RegisterDeviceCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device,
                                                             GameInputKind kind, GameInputDeviceStatus filter,
                                                             GameInputEnumerationKind enumeration_kind, void *context,
                                                             GameInputDeviceCallback_v2 callback,
                                                             GameInputCallbackToken *token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device_callback_v2_job *job = NULL;
    struct device_callback_v2 *entry = NULL;
    HRESULT hr;

    TRACE( "impl %p, device %p, kind %#x, filter %#x, enumeration_kind %#x, context %p, callback %p, token %p.\n",
           impl, device, kind, filter, enumeration_kind, context, callback, token );
    if (!callback || !token) return E_POINTER;
    *token = 0;
    if (enumeration_kind != GameInputNoEnumeration && enumeration_kind != GameInputAsyncEnumeration &&
        enumeration_kind != GameInputBlockingEnumeration)
        return E_INVALIDARG;
    if (FAILED(hr = device_callback_v2_create( device, kind, filter, context, callback, &entry ))) return hr;

    EnterCriticalSection( &game_input_cs );
    list_add_tail( &impl->callbacks_v2, &entry->entry );
    *token = (UINT_PTR)entry;
    if (enumeration_kind != GameInputNoEnumeration &&
        !(job = device_callback_v2_job_create_locked( impl, entry )))
    {
        list_remove( &entry->entry );
        entry->stopped = TRUE;
        *token = 0;
    }
    LeaveCriticalSection( &game_input_cs );

    if (enumeration_kind != GameInputNoEnumeration && !job)
    {
        device_callback_v2_release( entry );
        return E_OUTOFMEMORY;
    }
    if (enumeration_kind == GameInputBlockingEnumeration)
    {
        device_callback_v2_job_run( job );
        device_callback_v2_job_release( job );
    }
    else if (enumeration_kind == GameInputAsyncEnumeration &&
             !QueueUserWorkItem( device_callback_v2_worker, job, WT_EXECUTEDEFAULT ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        EnterCriticalSection( &game_input_cs );
        list_remove( &entry->entry );
        entry->stopped = TRUE;
        *token = 0;
        LeaveCriticalSection( &game_input_cs );
        device_callback_v2_job_release( job );
        device_callback_v2_release( entry );
        return hr;
    }
    return S_OK;
}

static HRESULT WINAPI game_input_v2_RegisterSystemButtonCallback( IGameInput_v2 *iface,
                                                                   IGameInputDevice_v2 *device,
                                                                   GameInputSystemButtons filter, void *context,
                                                                   GameInputSystemButtonCallback_v2 callback,
                                                                   GameInputCallbackToken *token )
{
    TRACE( "impl %p, device %p, filter %#x, context %p, callback %p, token %p.\n",
           impl_from_v2_IGameInput( iface ), device, filter, context, callback, token );
    if (token) *token = 0;
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_RegisterKeyboardLayoutCallback( IGameInput_v2 *iface,
                                                                    IGameInputDevice_v2 *device, void *context,
                                                                    GameInputKeyboardLayoutCallback_v2 callback,
                                                                    GameInputCallbackToken *token )
{
    TRACE( "impl %p, device %p, context %p, callback %p, token %p.\n",
           impl_from_v2_IGameInput( iface ), device, context, callback, token );
    if (token) *token = 0;
    return E_NOTIMPL;
}

static void WINAPI game_input_v2_StopCallback( IGameInput_v2 *iface, GameInputCallbackToken token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device_callback_v2 *entry;

    TRACE( "impl %p, token %I64u.\n", impl, token );
    EnterCriticalSection( &game_input_cs );
    LIST_FOR_EACH_ENTRY( entry, &impl->callbacks_v2, struct device_callback_v2, entry )
    {
        if (token != (UINT_PTR)entry) continue;
        entry->stopped = TRUE;
        break;
    }
    LeaveCriticalSection( &game_input_cs );
}

static bool WINAPI game_input_v2_UnregisterCallback( IGameInput_v2 *iface, GameInputCallbackToken token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device_callback_v2 *entry, *found = NULL;

    TRACE( "impl %p, token %I64u.\n", impl, token );
    EnterCriticalSection( &game_input_cs );
    LIST_FOR_EACH_ENTRY( entry, &impl->callbacks_v2, struct device_callback_v2, entry )
    {
        if (token != (UINT_PTR)entry) continue;
        list_remove( &entry->entry );
        entry->stopped = TRUE;
        found = entry;
        break;
    }
    LeaveCriticalSection( &game_input_cs );

    if (!found) return FALSE;
    WaitForSingleObject( found->idle_event, INFINITE );
    device_callback_v2_release( found );
    return TRUE;
}

static HRESULT WINAPI game_input_v2_CreateDispatcher( IGameInput_v2 *iface, IGameInputDispatcher **dispatcher )
{
    TRACE( "impl %p, dispatcher %p.\n", impl_from_v2_IGameInput( iface ), dispatcher );
    if (dispatcher) *dispatcher = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_FindDeviceFromId( IGameInput_v2 *iface, const APP_LOCAL_DEVICE_ID *value,
                                                       IGameInputDevice_v2 **device )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );

    if (!device) return E_POINTER;
    *device = NULL;
    if (!value || memcmp( value, &impl->mouse_device->info_v2.deviceId, sizeof(*value) ))
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    device_addref( impl->mouse_device );
    *device = &impl->mouse_device->IGameInputDevice_v2_iface;
    return S_OK;
}

static HRESULT WINAPI game_input_v2_FindDeviceFromPlatformString( IGameInput_v2 *iface, const WCHAR *value,
                                                                   IGameInputDevice_v2 **device )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );

    if (!device) return E_POINTER;
    *device = NULL;
    if (!value || wcscmp( value, impl->mouse_device->path )) return GAMEINPUT_E_DEVICE_NOT_FOUND;
    device_addref( impl->mouse_device );
    *device = &impl->mouse_device->IGameInputDevice_v2_iface;
    return S_OK;
}

static void WINAPI game_input_v2_SetFocusPolicy( IGameInput_v2 *iface, GameInputFocusPolicy policy )
{
    TRACE( "impl %p, policy %#x.\n", impl_from_v2_IGameInput( iface ), policy );
}

static const IGameInput_v2Vtbl game_input_v2_vtbl =
{
    game_input_v2_QueryInterface,
    game_input_v2_AddRef,
    game_input_v2_Release,
    game_input_v2_GetCurrentTimestamp,
    game_input_v2_GetCurrentReading,
    game_input_v2_GetNextReading,
    game_input_v2_GetPreviousReading,
    game_input_v2_RegisterReadingCallback,
    game_input_v2_RegisterDeviceCallback,
    game_input_v2_RegisterSystemButtonCallback,
    game_input_v2_RegisterKeyboardLayoutCallback,
    game_input_v2_StopCallback,
    game_input_v2_UnregisterCallback,
    game_input_v2_CreateDispatcher,
    game_input_v2_FindDeviceFromId,
    game_input_v2_FindDeviceFromPlatformString,
    game_input_v2_SetFocusPolicy,
};

static struct device *find_device( struct list *devices, const DEV_OBJECT *object )
{
    struct device *device;

    LIST_FOR_EACH_ENTRY( device, devices, struct device, entry )
        if (!wcscmp( device->path, object->pszObjectId )) return device;
    return NULL;
}

static void WINAPI device_query_cb( HDEVQUERY devquery, void *context, const DEV_QUERY_RESULT_ACTION_DATA *action )
{
    struct device_callbacks_v2_dispatch *dispatch;
    struct game_input *impl = context;
    struct device *device;

    switch (action->Action)
    {
    case DevQueryResultStateChange:
        TRACE( "impl %p, DevQueryResultStateChange %u\n", impl, action->Data.State );
        SetEvent( impl->initialized );
        break;

    case DevQueryResultAdd:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );

        dispatch = NULL;
        EnterCriticalSection( &game_input_cs );
        if ((device = find_device( &impl->devices, &action->Data.DeviceObject )) ||
            (device = device_create( &impl->devices, &action->Data.DeviceObject )))
        {
            GameInputDeviceStatus old_status = device->status;
            struct device_callback *entry;
            device->status = GameInputDeviceConnected;

            LIST_FOR_EACH_ENTRY( entry, &impl->callbacks, struct device_callback, entry )
                device_callback_notify( entry, &device->IGameInputDevice_v0_iface, GameInputKindGamepad,
                                        device->status, old_status );
            dispatch = device_callbacks_v2_dispatch_create_locked( impl, device, old_status );
        }
        LeaveCriticalSection( &game_input_cs );
        device_callbacks_v2_dispatch_run( dispatch );

        break;

    case DevQueryResultRemove:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );

        dispatch = NULL;
        EnterCriticalSection( &game_input_cs );
        if ((device = find_device( &impl->devices, &action->Data.DeviceObject )))
        {
            GameInputDeviceStatus old_status = device->status;
            struct device_callback *entry;
            device->status = GameInputDeviceNoStatus;

            LIST_FOR_EACH_ENTRY( entry, &impl->callbacks, struct device_callback, entry )
                device_callback_notify( entry, &device->IGameInputDevice_v0_iface, GameInputKindGamepad,
                                        device->status, old_status );
            dispatch = device_callbacks_v2_dispatch_create_locked( impl, device, old_status );
        }
        LeaveCriticalSection( &game_input_cs );
        device_callbacks_v2_dispatch_run( dispatch );

        break;

    case DevQueryResultUpdate:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );
        break;
    }
}

static struct game_input *game_input_create(void)
{
    struct game_input *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IGameInput_v0_iface.lpVtbl = &game_input_v0_vtbl;
    impl->IGameInput_v2_iface.lpVtbl = &game_input_v2_vtbl;
    impl->refcount = 1;
    list_init( &impl->devices );
    list_init( &impl->callbacks );
    list_init( &impl->callbacks_v2 );

    if (!(impl->mouse_device = mouse_device_create( &impl->devices )))
    {
        free( impl );
        return NULL;
    }

    if (!(impl->initialized = CreateEventW( NULL, TRUE, FALSE, NULL )) ||
        FAILED(DevCreateObjectQuery( DevObjectTypeDeviceInterface, DevQueryFlagUpdateResults | DevQueryFlagAllProperties,
                                     0, NULL, 0, NULL, device_query_cb, impl, &impl->query )))
    {
        if (impl->initialized) CloseHandle( impl->initialized );
        list_remove( &impl->mouse_device->entry );
        IGameInputDevice_v0_Release( &impl->mouse_device->IGameInputDevice_v0_iface );
        ERR( "Failed to enumerate devices\n" );
        free( impl );
        return NULL;
    }

    TRACE( "created GameInput %p\n", impl );
    return impl;
}

HRESULT WINAPI GameInputCreate( IGameInput_v0 **out )
{
    struct game_input *impl;

    TRACE( "out %p\n", out );

    if (!out) return E_POINTER;
    *out = NULL;

    EnterCriticalSection( &game_input_cs );
    if ((impl = game_input)) impl->refcount++;
    else impl = game_input = game_input_create();
    LeaveCriticalSection( &game_input_cs );
    if (!impl) return E_OUTOFMEMORY;

    WaitForSingleObject( impl->initialized, INFINITE );
    *out = &impl->IGameInput_v0_iface;
    return S_OK;
}

HRESULT WINAPI GameInputInitialize( REFIID iid, void **out )
{
    IGameInput_v0 *legacy;
    HRESULT hr;

    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (!out) return E_POINTER;
    *out = NULL;
    if (!iid) return E_NOINTERFACE;

    if (FAILED(hr = GameInputCreate( &legacy ))) return hr;
    hr = IGameInput_v0_QueryInterface( legacy, iid, out );
    IGameInput_v0_Release( legacy );
    return hr;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}
