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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
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

/*
 * Mouse tracking.
 *
 * GameInput reports mouse motion as monotonically accumulating relative counts
 * which the caller differentiates between readings, so the cursor position is
 * not a usable source: a title that has taken mouse look will pin or hide the
 * cursor and every delta past the screen edge would be lost. WM_INPUT gives the
 * raw counts directly and keeps working while the pointer is clipped, so a
 * message-only window on a dedicated thread accumulates them here.
 */
static struct
{
    LONG64 position_x, position_y;   /* free-running relative counts */
    LONG64 wheel_x, wheel_y;
    LONG buttons;
    LONG last_absolute_x, last_absolute_y;  /* only for MOUSE_MOVE_ABSOLUTE deltas */
} mouse_state;

static HANDLE mouse_thread;
static HWND mouse_window;

#ifndef RI_MOUSE_HWHEEL
#define RI_MOUSE_HWHEEL 0x0800  /* horizontal wheel; absent from this winuser.h */
#endif

static void mouse_update_buttons( USHORT flags )
{
    static const struct { USHORT down, up; LONG bit; } map[] =
    {
        { RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   GameInputMouseLeftButton },
        { RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  GameInputMouseRightButton },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, GameInputMouseMiddleButton },
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(map); i++)
    {
        if (flags & map[i].down) InterlockedOr( &mouse_state.buttons, map[i].bit );
        if (flags & map[i].up) InterlockedAnd( &mouse_state.buttons, ~map[i].bit );
    }
}

static LRESULT CALLBACK mouse_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_INPUT)
    {
        UINT size = sizeof(RAWINPUT);
        RAWINPUT ri;

        if (GetRawInputData( (HRAWINPUT)lparam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER) ) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE *mouse = &ri.data.mouse;

            if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE)
            {
                /* Absolute devices (tablets, some remote sessions) report a
                 * position rather than a delta; derive the delta ourselves so
                 * callers still see continuous relative motion. */
                LONG prev_x = InterlockedExchange( &mouse_state.last_absolute_x, mouse->lLastX );
                LONG prev_y = InterlockedExchange( &mouse_state.last_absolute_y, mouse->lLastY );
                InterlockedAdd64( &mouse_state.position_x, mouse->lLastX - prev_x );
                InterlockedAdd64( &mouse_state.position_y, mouse->lLastY - prev_y );
            }
            else
            {
                InterlockedAdd64( &mouse_state.position_x, mouse->lLastX );
                InterlockedAdd64( &mouse_state.position_y, mouse->lLastY );
            }

            mouse_update_buttons( mouse->usButtonFlags );

            /* Accumulate the raw wheel value, which is WHEEL_DELTA (120) per
             * detent. Dividing down to whole notches here looks harmless --
             * anything that only checks the sign of the delta, such as hotbar
             * cycling, behaves identically -- but callers that scale a scroll
             * distance by the magnitude then move 1/120th as far, which is
             * what made menu scrolling crawl. */
            if (mouse->usButtonFlags & RI_MOUSE_WHEEL)
                InterlockedAdd64( &mouse_state.wheel_y, (SHORT)mouse->usButtonData );
            if (mouse->usButtonFlags & RI_MOUSE_HWHEEL)
                InterlockedAdd64( &mouse_state.wheel_x, (SHORT)mouse->usButtonData );
        }
        return 0;
    }

    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static DWORD CALLBACK mouse_thread_proc( void *arg )
{
    WNDCLASSEXW class = { .cbSize = sizeof(class), .lpfnWndProc = mouse_wndproc,
                          .hInstance = GetModuleHandleW( NULL ), .lpszClassName = L"__wine_gameinput_mouse" };
    RAWINPUTDEVICE rid = { .usUsagePage = HID_USAGE_PAGE_GENERIC, .usUsage = HID_USAGE_GENERIC_MOUSE };
    MSG msg;

    RegisterClassExW( &class );

    /* HWND_MESSAGE: invisible, never activated, and still receives WM_INPUT. */
    if (!(mouse_window = CreateWindowExW( 0, class.lpszClassName, NULL, 0, 0, 0, 0, 0,
                                          HWND_MESSAGE, NULL, class.hInstance, NULL )))
    {
        ERR( "failed to create mouse window, error %lu\n", GetLastError() );
        return 0;
    }

    /* RIDEV_INPUTSINK so counts keep arriving even when the game window is not
     * the foreground window; without it mouse look stalls on focus changes. */
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = mouse_window;
    if (!RegisterRawInputDevices( &rid, 1, sizeof(rid) ))
        ERR( "failed to register for raw mouse input, error %lu\n", GetLastError() );

    while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    return 0;
}

static void mouse_tracking_start(void)
{
    if (mouse_thread) return;
    if (!(mouse_thread = CreateThread( NULL, 0, mouse_thread_proc, NULL, 0, NULL )))
        ERR( "failed to start mouse thread, error %lu\n", GetLastError() );
}

/*
 * GameInput is a versioned API: every SDK revision defines a parallel set of
 * interfaces (IGameInput_v0/_v1/_v2/_v3 plus matching device and reading
 * interfaces). They are not derived from one another and do not share a method
 * layout, so they cannot be aliased onto each other. A title asks
 * GameInputCreate for the revision it was built against and gives up if that
 * QueryInterface fails.
 *
 * Only _v0 used to be implemented, so anything built against a newer SDK --
 * Minecraft Bedrock asks for IGameInput_v2 -- was refused with E_NOINTERFACE
 * and reported the runtime as missing. The v2 interfaces below are a second
 * face on the same objects rather than a wrapper: device list, HID enumeration
 * and callback plumbing are shared and only the vtables differ.
 */

struct device
{
    IGameInputDevice_v0 IGameInputDevice_v0_iface;
    IGameInputDevice_v2 IGameInputDevice_v2_iface;
    LONG refcount;
    WCHAR path[MAX_PATH];
    struct list entry;

    GameInputDeviceStatus status;
    GameInputKind kind;
    GameInputDeviceInfo_v0 info_v0;
    GameInputDeviceInfo_v2 info_v2;
    GameInputMouseInfo_v2 mouse_info;
};

static struct device *device_from_IGameInputDevice_v0( IGameInputDevice_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct device, IGameInputDevice_v0_iface );
}

static struct device *device_from_IGameInputDevice_v2( IGameInputDevice_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct device, IGameInputDevice_v2_iface );
}

/* Both faces share a single refcount, so either may be handed out for either
 * IID as long as the reference is taken once. */
static HRESULT device_query_interface( struct device *device, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IGameInputDevice_v0 ) ||
        IsEqualGUID( iid, &IID_IUnknown ))
    {
        IGameInputDevice_v0_AddRef( &device->IGameInputDevice_v0_iface );
        *out = &device->IGameInputDevice_v0_iface;
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameInputDevice_v2 ))
    {
        IGameInputDevice_v0_AddRef( &device->IGameInputDevice_v0_iface );
        *out = &device->IGameInputDevice_v2_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static HRESULT WINAPI game_input_device_v0_QueryInterface( IGameInputDevice_v0 *iface, REFIID iid, void **out )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );

    TRACE( "device %p, iid %s, out %p.\n", device, debugstr_guid( iid ), out );

    return device_query_interface( device, iid, out );
}

static ULONG WINAPI game_input_device_v0_AddRef( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    ULONG ref = InterlockedIncrement( &device->refcount );
    TRACE( "device %p increasing refcount to %lu.\n", device, ref );
    return ref;
}

static ULONG WINAPI game_input_device_v0_Release( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    ULONG ref = InterlockedDecrement( &device->refcount );
    TRACE( "device %p decreasing refcount to %lu.\n", device, ref );
    if (!ref) free( device );
    return ref;
};

static const GameInputDeviceInfo_v0 *WINAPI game_input_device_v0_GetDeviceInfo( IGameInputDevice_v0 *iface )
{
    struct device *device = device_from_IGameInputDevice_v0( iface );
    FIXME( "device %p stub!\n", device );
    return &device->info_v0;
}

static GameInputDeviceStatus WINAPI game_input_device_v0_GetDeviceStatus( IGameInputDevice_v0 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v0( iface ) );
    return 0;
}

static void WINAPI game_input_device_v0_GetBatteryState( IGameInputDevice_v0 *iface, GameInputBatteryState *state )
{
    FIXME( "device %p, state %p stub!\n", device_from_IGameInputDevice_v0( iface ), state );
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
    struct device *device = device_from_IGameInputDevice_v2( iface );
    return IGameInputDevice_v0_AddRef( &device->IGameInputDevice_v0_iface );
}

static ULONG WINAPI game_input_device_v2_Release( IGameInputDevice_v2 *iface )
{
    struct device *device = device_from_IGameInputDevice_v2( iface );
    return IGameInputDevice_v0_Release( &device->IGameInputDevice_v0_iface );
}

/* Unlike v0, which returns the pointer directly, v2 reports it through an out
 * parameter and an HRESULT. */
static HRESULT WINAPI game_input_device_v2_GetDeviceInfo( IGameInputDevice_v2 *iface, const GameInputDeviceInfo_v2 **info )
{
    struct device *device = device_from_IGameInputDevice_v2( iface );

    FIXME( "device %p, info %p stub!\n", device, info );

    if (!info) return E_POINTER;
    *info = &device->info_v2;
    return S_OK;
}

static HRESULT WINAPI game_input_device_v2_GetHapticInfo( IGameInputDevice_v2 *iface, GameInputHapticInfo *info )
{
    FIXME( "device %p, info %p stub!\n", device_from_IGameInputDevice_v2( iface ), info );
    return E_NOTIMPL;
}

static GameInputDeviceStatus WINAPI game_input_device_v2_GetDeviceStatus( IGameInputDevice_v2 *iface )
{
    FIXME( "device %p stub!\n", device_from_IGameInputDevice_v2( iface ) );
    return 0;
}

static HRESULT WINAPI game_input_device_v2_CreateForceFeedbackEffect( IGameInputDevice_v2 *iface, uint32_t motor,
                                                                      const GameInputForceFeedbackParams *params,
                                                                      IGameInputForceFeedbackEffect_v2 **effect )
{
    FIXME( "device %p, motor %u, params %p, effect %p stub!\n", device_from_IGameInputDevice_v2( iface ), motor, params, effect );
    return E_NOTIMPL;
}

static bool WINAPI game_input_device_v2_IsForceFeedbackMotorPoweredOn( IGameInputDevice_v2 *iface, uint32_t motor )
{
    FIXME( "device %p, motor %u stub!\n", device_from_IGameInputDevice_v2( iface ), motor );
    return false;
}

static void WINAPI game_input_device_v2_SetForceFeedbackMotorGain( IGameInputDevice_v2 *iface, uint32_t motor, float gain )
{
    FIXME( "device %p, motor %u, gain %f stub!\n", device_from_IGameInputDevice_v2( iface ), motor, gain );
}

static void WINAPI game_input_device_v2_SetRumbleState( IGameInputDevice_v2 *iface, const GameInputRumbleParams *params )
{
    FIXME( "device %p, params %p stub!\n", device_from_IGameInputDevice_v2( iface ), params );
}

static HRESULT WINAPI game_input_device_v2_DirectInputEscape( IGameInputDevice_v2 *iface, uint32_t command, const void *input,
                                                              uint32_t in_size, void *output, uint32_t out_size, uint32_t *size )
{
    FIXME( "device %p, command %u, input %p, in_size %u, output %p, out_size %u, size %p stub!\n",
           device_from_IGameInputDevice_v2( iface ), command, input, in_size, output, out_size, size );
    return E_NOTIMPL;
}

static const IGameInputDevice_v2Vtbl game_input_device_v2_vtbl =
{
    game_input_device_v2_QueryInterface,
    game_input_device_v2_AddRef,
    game_input_device_v2_Release,
    /* IGameInputDevice_v2 methods */
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

    /* Mice matter as much as gamepads here: titles ask GameInput whether a
     * mouse exists before using it for look/aim, and Minecraft Bedrock takes
     * its keyboard through Win32 but its mouse through GameInput. Rejecting
     * every non-gamepad left such a title with no pointer input at all. */
    if (caps.UsagePage != HID_USAGE_PAGE_GENERIC) goto done;
    if (caps.Usage != HID_USAGE_GENERIC_GAMEPAD && caps.Usage != HID_USAGE_GENERIC_MOUSE) goto done;
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

    /* Same device described through the v2 layout. The structs are not
     * compatible -- v2 has no infoSize or capabilities field, among other
     * differences -- so it is filled separately rather than cast. */
    device->info_v2.vendorId = attr.VendorID;
    device->info_v2.productId = attr.ProductID;
    device->info_v2.usage.page = caps.UsagePage;
    device->info_v2.usage.id = caps.Usage;
    device->info_v2.deviceFamily = family;
    device->info_v2.supportedRumbleMotors = GameInputRumbleNone;
    device->info_v2.controllerAxisCount = 6;
    device->info_v2.controllerButtonCount = caps.NumberInputButtonCaps;
    device->info_v2.controllerSwitchCount = 1;
    device->info_v2.supportedSystemButtons = GameInputSystemButtonGuide;

    if (caps.Usage == HID_USAGE_GENERIC_MOUSE)
    {
        device->kind = GameInputKindMouse;
        device->mouse_info.supportedButtons = GameInputMouseLeftButton | GameInputMouseRightButton |
                                              GameInputMouseMiddleButton;
        device->mouse_info.hasWheelX = TRUE;
        device->mouse_info.hasWheelY = TRUE;
        device->info_v2.supportedInput = GameInputKindMouse;
        device->info_v2.mouseInfo = &device->mouse_info;
        mouse_tracking_start();
    }
    else
    {
        device->kind = GameInputKindGamepad | GameInputKindController;
        device->info_v2.supportedInput = GameInputKindGamepad | GameInputKindController;
    }

    list_add_tail( devices, &device->entry );

    TRACE( "created device %p\n", device );
done:
    CloseHandle( file );
    return device;
}

/*
 * A registration remembers which interface revision it was made through: the
 * callback signatures differ only in the device pointer they are handed, so a
 * v2 registration must be given the device's v2 face. Filtering is done on the
 * underlying object, which is shared, so it is version independent.
 */
struct device_callback
{
    struct list entry;

    struct device *device;
    unsigned int version;
    GameInputKind input_kind;
    GameInputDeviceStatus status_filter;

    void *context;
    union
    {
        GameInputDeviceCallback_v0 v0;
        GameInputDeviceCallback_v2 v2;
    } callback;
};

static void device_callback_notify( struct device_callback *entry, struct device *device, GameInputKind input_kind,
                                    GameInputDeviceStatus new_status, GameInputDeviceStatus old_status )
{
    if (entry->device && entry->device != device) return;
    if (!(entry->input_kind & input_kind)) return;
    if (!(entry->status_filter & (new_status | old_status))) return;

    if (entry->version >= 2)
        entry->callback.v2( (UINT_PTR)entry, entry->context, &device->IGameInputDevice_v2_iface,
                            0, new_status, old_status );
    else
        entry->callback.v0( (UINT_PTR)entry, entry->context, &device->IGameInputDevice_v0_iface,
                            0, new_status, old_status );
}

static HRESULT device_callback_create( struct device *device, unsigned int version, GameInputKind input_kind,
                                       GameInputDeviceStatus status_filter, void *context, void *callback,
                                       struct device_callback **out )
{
    struct device_callback *entry;

    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    if ((entry->device = device)) IGameInputDevice_v0_AddRef( &device->IGameInputDevice_v0_iface );
    entry->version = version;
    entry->input_kind = input_kind;
    entry->status_filter = status_filter;
    entry->context = context;
    if (version >= 2) entry->callback.v2 = callback;
    else entry->callback.v0 = callback;

    *out = entry;
    return S_OK;
}

/*
 * A reading is an immutable snapshot of one device's state at one instant.
 * Callers hold on to them and diff consecutive readings, which is why the
 * mouse counters must be free-running totals rather than per-frame deltas.
 */
struct reading
{
    IGameInputReading_v2 IGameInputReading_v2_iface;
    LONG refcount;
    struct device *device;
    GameInputKind kind;
    uint64_t timestamp;
    GameInputMouseState_v2 mouse;
};

static uint64_t game_input_timestamp(void)
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceCounter( &counter );
    QueryPerformanceFrequency( &frequency );
    if (!frequency.QuadPart) return 0;
    /* GameInput timestamps are microseconds. */
    return counter.QuadPart / (frequency.QuadPart / 1000000);
}

static struct reading *reading_from_IGameInputReading_v2( IGameInputReading_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct reading, IGameInputReading_v2_iface );
}

static HRESULT WINAPI reading_v2_QueryInterface( IGameInputReading_v2 *iface, REFIID iid, void **out )
{
    struct reading *impl = reading_from_IGameInputReading_v2( iface );

    TRACE( "reading %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInputReading_v2 ))
    {
        IGameInputReading_v2_AddRef( &impl->IGameInputReading_v2_iface );
        *out = &impl->IGameInputReading_v2_iface;
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI reading_v2_AddRef( IGameInputReading_v2 *iface )
{
    struct reading *impl = reading_from_IGameInputReading_v2( iface );
    return InterlockedIncrement( &impl->refcount );
}

static ULONG WINAPI reading_v2_Release( IGameInputReading_v2 *iface )
{
    struct reading *impl = reading_from_IGameInputReading_v2( iface );
    ULONG ref = InterlockedDecrement( &impl->refcount );

    if (!ref)
    {
        if (impl->device) IGameInputDevice_v0_Release( &impl->device->IGameInputDevice_v0_iface );
        free( impl );
    }
    return ref;
}

static GameInputKind WINAPI reading_v2_GetInputKind( IGameInputReading_v2 *iface )
{
    return reading_from_IGameInputReading_v2( iface )->kind;
}

static uint64_t WINAPI reading_v2_GetTimestamp( IGameInputReading_v2 *iface )
{
    return reading_from_IGameInputReading_v2( iface )->timestamp;
}

static void WINAPI reading_v2_GetDevice( IGameInputReading_v2 *iface, IGameInputDevice_v2 **device )
{
    struct reading *impl = reading_from_IGameInputReading_v2( iface );

    if (!device) return;
    if ((*device = impl->device ? &impl->device->IGameInputDevice_v2_iface : NULL))
        IGameInputDevice_v2_AddRef( *device );
}

static uint32_t WINAPI reading_v2_GetControllerAxisCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetControllerAxisState( IGameInputReading_v2 *iface, uint32_t count, float *state )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetControllerButtonCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetControllerButtonState( IGameInputReading_v2 *iface, uint32_t count, bool *state )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetControllerSwitchCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetControllerSwitchState( IGameInputReading_v2 *iface, uint32_t count, GameInputSwitchPosition *state )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetKeyCount( IGameInputReading_v2 *iface )
{
    return 0;
}

static uint32_t WINAPI reading_v2_GetKeyState( IGameInputReading_v2 *iface, uint32_t count, GameInputKeyState *state )
{
    return 0;
}

static bool WINAPI reading_v2_GetMouseState( IGameInputReading_v2 *iface, GameInputMouseState_v2 *state )
{
    struct reading *impl = reading_from_IGameInputReading_v2( iface );

    if (!state || !(impl->kind & GameInputKindMouse)) return false;
    *state = impl->mouse;
    return true;
}

static bool WINAPI reading_v2_GetSensorsState( IGameInputReading_v2 *iface, GameInputSensorsState *state )
{
    return false;
}

static bool WINAPI reading_v2_GetArcadeStickState( IGameInputReading_v2 *iface, GameInputArcadeStickState *state )
{
    return false;
}

static bool WINAPI reading_v2_GetFlightStickState( IGameInputReading_v2 *iface, GameInputFlightStickState *state )
{
    return false;
}

static bool WINAPI reading_v2_GetGamepadState( IGameInputReading_v2 *iface, GameInputGamepadState *state )
{
    return false;
}

static bool WINAPI reading_v2_GetRacingWheelState( IGameInputReading_v2 *iface, GameInputRacingWheelState *state )
{
    return false;
}

static bool WINAPI reading_v2_GetUiNavigationState( IGameInputReading_v2 *iface, GameInputUiNavigationState *state )
{
    return false;
}

static const IGameInputReading_v2Vtbl reading_v2_vtbl =
{
    reading_v2_QueryInterface,
    reading_v2_AddRef,
    reading_v2_Release,
    /* IGameInputReading_v2 methods */
    reading_v2_GetInputKind,
    reading_v2_GetTimestamp,
    reading_v2_GetDevice,
    reading_v2_GetControllerAxisCount,
    reading_v2_GetControllerAxisState,
    reading_v2_GetControllerButtonCount,
    reading_v2_GetControllerButtonState,
    reading_v2_GetControllerSwitchCount,
    reading_v2_GetControllerSwitchState,
    reading_v2_GetKeyCount,
    reading_v2_GetKeyState,
    reading_v2_GetMouseState,
    reading_v2_GetSensorsState,
    reading_v2_GetArcadeStickState,
    reading_v2_GetFlightStickState,
    reading_v2_GetGamepadState,
    reading_v2_GetRacingWheelState,
    reading_v2_GetUiNavigationState,
};

/* Snapshot the accumulated mouse counters into a fresh reading. */
static HRESULT mouse_reading_create( struct device *device, IGameInputReading_v2 **out )
{
    GameInputMousePositions positions = GameInputMouseRelativePosition;
    struct reading *impl;
    POINT point;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IGameInputReading_v2_iface.lpVtbl = &reading_v2_vtbl;
    impl->refcount = 1;
    impl->kind = GameInputKindMouse;
    impl->timestamp = game_input_timestamp();
    if ((impl->device = device)) IGameInputDevice_v0_AddRef( &device->IGameInputDevice_v0_iface );

    impl->mouse.buttons = ReadAcquire( &mouse_state.buttons );
    impl->mouse.positionX = ReadAcquire64( &mouse_state.position_x );
    impl->mouse.positionY = ReadAcquire64( &mouse_state.position_y );
    impl->mouse.wheelX = ReadAcquire64( &mouse_state.wheel_x );
    impl->mouse.wheelY = ReadAcquire64( &mouse_state.wheel_y );

    /* The absolute position has to be a real pointer location, not a running
     * total of deltas: in-game look only differentiates positionX/Y, but menus
     * place a cursor from absolutePositionX/Y and a synthetic origin puts it
     * nowhere near the pointer. GetCursorPos is the authority for that. */
    if (GetCursorPos( &point ))
    {
        impl->mouse.absolutePositionX = point.x;
        impl->mouse.absolutePositionY = point.y;
        positions |= GameInputMouseAbsolutePosition;
    }

    /* Without these flags the caller is being told neither position field is
     * meaningful, which is what left menu cursors dead. */
    impl->mouse.positions = positions;

    *out = &impl->IGameInputReading_v2_iface;
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
};

static struct game_input *impl_from_v0_IGameInput( IGameInput_v0 *iface )
{
    return CONTAINING_RECORD( iface, struct game_input, IGameInput_v0_iface );
}

static struct game_input *impl_from_v2_IGameInput( IGameInput_v2 *iface )
{
    return CONTAINING_RECORD( iface, struct game_input, IGameInput_v2_iface );
}

static HRESULT game_input_query_interface( struct game_input *impl, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IGameInput_v0 ))
    {
        IGameInput_v0_AddRef( &impl->IGameInput_v0_iface );
        *out = &impl->IGameInput_v0_iface;
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameInput_v2 ))
    {
        IGameInput_v0_AddRef( &impl->IGameInput_v0_iface );
        *out = &impl->IGameInput_v2_iface;
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI game_input_v0_QueryInterface( IGameInput_v0 *iface, REFIID iid, void **out )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );

    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    return game_input_query_interface( impl, iid, out );
}

static ULONG WINAPI game_input_v0_AddRef( IGameInput_v0 *iface )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );
    ULONG ref;

    EnterCriticalSection( &game_input_cs );
    ref = ++impl->refcount;
    LeaveCriticalSection( &game_input_cs );

    TRACE( "impl %p increasing refcount to %lu.\n", impl, ref );

    return ref;
}

static ULONG WINAPI game_input_v0_Release( IGameInput_v0 *iface )
{
    struct game_input *impl = impl_from_v0_IGameInput( iface );
    ULONG ref;

    EnterCriticalSection( &game_input_cs );
    if (!(ref = --impl->refcount)) game_input = NULL;
    LeaveCriticalSection( &game_input_cs );

    TRACE( "impl %p decreasing refcount to %lu.\n", impl, ref );

    if (!ref)
    {
        struct list *ptr;

        while ((ptr = list_head( &impl->callbacks )))
        {
            struct device_callback *callback = LIST_ENTRY(ptr, struct device_callback, entry);
            list_remove( &callback->entry );
            free( callback );
        }

        while ((ptr = list_head( &impl->devices )))
        {
            struct device *device = LIST_ENTRY(ptr, struct device, entry);
            list_remove( &device->entry );
            IGameInputDevice_v0_Release( &device->IGameInputDevice_v0_iface );
        }

        DevCloseObjectQuery( impl->query );
        CloseHandle( impl->initialized );
        free( impl );
    }

    return ref;
}

static uint64_t WINAPI game_input_v0_GetCurrentTimestamp( IGameInput_v0 *iface )
{
    FIXME( "impl %p stub!\n", impl_from_v0_IGameInput( iface ) );
    return 0;
}

static HRESULT WINAPI game_input_v0_GetCurrentReading( IGameInput_v0 *iface, GameInputKind input_kind,
                                                       IGameInputDevice_v0 *device_iface, IGameInputReading_v0 **reading )
{
    FIXME( "impl %p, input_kind %#x, device %p, reading %p\n", impl_from_v0_IGameInput( iface ), input_kind, device_iface, reading );
    return E_NOTIMPL;
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

    if (FAILED(hr = device_callback_create( device_iface ? device_from_IGameInputDevice_v0( device_iface ) : NULL,
                                            0, input_kind, status_filter, context, callback, &entry )))
        return hr;

    EnterCriticalSection( &game_input_cs );
    list_add_tail( &impl->callbacks, &entry->entry );

    LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry )
        device_callback_notify( entry, device, device->kind,
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
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return IGameInput_v0_AddRef( &impl->IGameInput_v0_iface );
}

static ULONG WINAPI game_input_v2_Release( IGameInput_v2 *iface )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return IGameInput_v0_Release( &impl->IGameInput_v0_iface );
}

static uint64_t WINAPI game_input_v2_GetCurrentTimestamp( IGameInput_v2 *iface )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return IGameInput_v0_GetCurrentTimestamp( &impl->IGameInput_v0_iface );
}

/* Readings are not implemented for any interface version yet; these mirror what
 * v0 reports rather than pretending v2 can do more. */
static HRESULT WINAPI game_input_v2_GetCurrentReading( IGameInput_v2 *iface, GameInputKind input_kind,
                                                       IGameInputDevice_v2 *device_iface, IGameInputReading_v2 **reading )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device *device, *mouse = NULL;

    TRACE( "impl %p, input_kind %#x, device %p, reading %p.\n", impl, input_kind, device_iface, reading );

    if (!reading) return E_POINTER;

    if (input_kind & GameInputKindMouse)
    {
        if (device_iface) mouse = device_from_IGameInputDevice_v2( device_iface );
        else
        {
            EnterCriticalSection( &game_input_cs );
            LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry )
                if (device->kind & GameInputKindMouse) { mouse = device; break; }
            LeaveCriticalSection( &game_input_cs );
        }

        if (mouse) return mouse_reading_create( mouse, reading );
    }

    /* Only mouse readings are produced so far; gamepad and keyboard readings
     * are still unimplemented for every interface version. */
    FIXME( "impl %p, input_kind %#x, device %p: unsupported reading kind.\n", impl, input_kind, device_iface );
    *reading = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_GetNextReading( IGameInput_v2 *iface, IGameInputReading_v2 *reference, GameInputKind input_kind,
                                                    IGameInputDevice_v2 *device, IGameInputReading_v2 **reading )
{
    FIXME( "impl %p, reference %p, input_kind %#x, device %p, reading %p stub!\n",
           impl_from_v2_IGameInput( iface ), reference, input_kind, device, reading );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_GetPreviousReading( IGameInput_v2 *iface, IGameInputReading_v2 *reference, GameInputKind input_kind,
                                                        IGameInputDevice_v2 *device, IGameInputReading_v2 **reading )
{
    FIXME( "impl %p, reference %p, input_kind %#x, device %p, reading %p stub!\n",
           impl_from_v2_IGameInput( iface ), reference, input_kind, device, reading );
    return E_NOTIMPL;
}

/* v2 dropped v0's analog_threshold parameter. */
static HRESULT WINAPI game_input_v2_RegisterReadingCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device, GameInputKind input_kind,
                                                             void *context, GameInputReadingCallback_v2 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, input_kind %#x, context %p, callback %p, token %p stub!\n",
           impl_from_v2_IGameInput( iface ), device, input_kind, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_RegisterDeviceCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device_iface, GameInputKind input_kind,
                                                            GameInputDeviceStatus status_filter, GameInputEnumerationKind enumeration_kind,
                                                            void *context, GameInputDeviceCallback_v2 callback, GameInputCallbackToken *token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    struct device_callback *entry;
    struct device *device;
    HRESULT hr;

    TRACE( "impl %p, device_iface %p, input_kind %#x, status_filter %#x, enumeration_kind %#x, context %p, callback %p, token %p.\n",
           impl, device_iface, input_kind, status_filter, enumeration_kind, context, callback, token );

    if (FAILED(hr = device_callback_create( device_iface ? device_from_IGameInputDevice_v2( device_iface ) : NULL,
                                            2, input_kind, status_filter, context, callback, &entry )))
        return hr;

    EnterCriticalSection( &game_input_cs );
    list_add_tail( &impl->callbacks, &entry->entry );

    /* Report the devices already enumerated, as a fresh registration expects. */
    LIST_FOR_EACH_ENTRY( device, &impl->devices, struct device, entry )
        device_callback_notify( entry, device, device->kind,
                                device->status, GameInputDeviceNoStatus );

    LeaveCriticalSection( &game_input_cs );

    *token = (UINT_PTR)entry;
    return hr;
}

static HRESULT WINAPI game_input_v2_RegisterSystemButtonCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device,
                                                                  GameInputSystemButtons button_filter, void *context,
                                                                  GameInputSystemButtonCallback_v2 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, button_filter %#x, context %p, callback %p, token %p stub!\n",
           impl_from_v2_IGameInput( iface ), device, button_filter, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_RegisterKeyboardLayoutCallback( IGameInput_v2 *iface, IGameInputDevice_v2 *device, void *context,
                                                                    GameInputKeyboardLayoutCallback_v2 callback, GameInputCallbackToken *token )
{
    FIXME( "impl %p, device %p, context %p, callback %p, token %p stub!\n",
           impl_from_v2_IGameInput( iface ), device, context, callback, token );
    return E_NOTIMPL;
}

static void WINAPI game_input_v2_StopCallback( IGameInput_v2 *iface, GameInputCallbackToken token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    IGameInput_v0_StopCallback( &impl->IGameInput_v0_iface, token );
}

/* v2 dropped v0's timeout argument; wait indefinitely, which is what a caller
 * with no timeout to give is asking for. */
static bool WINAPI game_input_v2_UnregisterCallback( IGameInput_v2 *iface, GameInputCallbackToken token )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return IGameInput_v0_UnregisterCallback( &impl->IGameInput_v0_iface, token, UINT64_MAX );
}

static HRESULT WINAPI game_input_v2_CreateDispatcher( IGameInput_v2 *iface, IGameInputDispatcher **dispatcher )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    return IGameInput_v0_CreateDispatcher( &impl->IGameInput_v0_iface, dispatcher );
}

static HRESULT WINAPI game_input_v2_FindDeviceFromId( IGameInput_v2 *iface, const APP_LOCAL_DEVICE_ID *value, IGameInputDevice_v2 **device )
{
    FIXME( "impl %p, value %p, device %p stub!\n", impl_from_v2_IGameInput( iface ), value, device );
    return E_NOTIMPL;
}

static HRESULT WINAPI game_input_v2_FindDeviceFromPlatformString( IGameInput_v2 *iface, const WCHAR *value, IGameInputDevice_v2 **device )
{
    FIXME( "impl %p, value %s, device %p stub!\n", impl_from_v2_IGameInput( iface ), debugstr_w( value ), device );
    return E_NOTIMPL;
}

static void WINAPI game_input_v2_SetFocusPolicy( IGameInput_v2 *iface, GameInputFocusPolicy policy )
{
    struct game_input *impl = impl_from_v2_IGameInput( iface );
    IGameInput_v0_SetFocusPolicy( &impl->IGameInput_v0_iface, policy );
}

static const IGameInput_v2Vtbl game_input_v2_vtbl =
{
    game_input_v2_QueryInterface,
    game_input_v2_AddRef,
    game_input_v2_Release,
    /* IGameInput_v2 methods */
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

        EnterCriticalSection( &game_input_cs );
        if ((device = find_device( &impl->devices, &action->Data.DeviceObject )) ||
            (device = device_create( &impl->devices, &action->Data.DeviceObject )))
        {
            GameInputDeviceStatus old_status = device->status;
            struct device_callback *entry;
            device->status = GameInputDeviceConnected;

            LIST_FOR_EACH_ENTRY( entry, &impl->callbacks, struct device_callback, entry )
                device_callback_notify( entry, device, device->kind,
                                        device->status, old_status );
        }
        LeaveCriticalSection( &game_input_cs );

        break;

    case DevQueryResultRemove:
        TRACE( "impl %p, action %u type %u id %s props %lu\n", impl, action->Action, action->Data.DeviceObject.ObjectType,
               debugstr_w(action->Data.DeviceObject.pszObjectId), action->Data.DeviceObject.cPropertyCount );

        EnterCriticalSection( &game_input_cs );
        if ((device = find_device( &impl->devices, &action->Data.DeviceObject )))
        {
            GameInputDeviceStatus old_status = device->status;
            struct device_callback *entry;
            device->status = GameInputDeviceNoStatus;

            LIST_FOR_EACH_ENTRY( entry, &impl->callbacks, struct device_callback, entry )
                device_callback_notify( entry, device, device->kind,
                                        device->status, old_status );
        }
        LeaveCriticalSection( &game_input_cs );

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

    if (!(impl->initialized = CreateEventW( NULL, TRUE, FALSE, NULL )) ||
        FAILED(DevCreateObjectQuery( DevObjectTypeDeviceInterface, DevQueryFlagUpdateResults | DevQueryFlagAllProperties,
                                     0, NULL, 0, NULL, device_query_cb, impl, &impl->query )))
    {
        if (impl->initialized) CloseHandle( impl->initialized );
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

    const char *sgi, *sd;
    if ((!(sgi = getenv( "SteamGameId" )) || strcmp(sgi, "1771300") /* Kingdom Come Deliverance II */)) goto failed;
    if ((sd = getenv( "SteamDeck" )) && atoi( sd )) goto failed; /* not on Steam Deck */

    TRACE( "out %p\n", out );

    EnterCriticalSection( &game_input_cs );
    if ((impl = game_input)) impl->refcount++;
    else impl = game_input = game_input_create();
    LeaveCriticalSection( &game_input_cs );
    if (!impl) return E_OUTOFMEMORY;

    WaitForSingleObject( impl->initialized, INFINITE );
    *out = &impl->IGameInput_v0_iface;
    return S_OK;

failed:
    FIXME( "out %p, stub!\n", out );
    return E_NOTIMPL;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}
