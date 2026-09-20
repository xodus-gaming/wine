/*
 * Copyright (C) 2023 Mohamad Al-Jaf
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

#include "initguid.h"
#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/***********************************************************************
 *           RegisterAppConstrainedChangeNotification (twinapi.appcore.@)
 */
ULONG WINAPI RegisterAppConstrainedChangeNotification( PAPPCONSTRAIN_CHANGE_ROUTINE routine, void *context, PAPPCONSTRAIN_REGISTRATION *reg )
{
    FIXME( "routine %p, context %p, reg %p - stub.\n", routine, context, reg );
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Opaque registration handle for RegisterAppStateChangeNotification().
 *
 * On real Windows this API always succeeds for a normal foreground desktop
 * app and returns a valid PAPPSTATE_REGISTRATION handle (the callback is
 * only ever invoked later, on suspend/resume, which never happens for an
 * unpackaged desktop process). GDK/XGameRuntime app bring-up treats a
 * non-ERROR_SUCCESS return from this call as fatal and aborts startup
 * (calls XGameRuntimeUninitialize + exit(0)), so returning
 * ERROR_CALL_NOT_IMPLEMENTED here - as the previous stub did - causes any
 * GDK title to quietly exit right after this call. See appnotify.h /
 * MSDN: "If this function succeeds, it returns ERROR_SUCCESS." */
struct app_state_registration
{
    PAPPSTATE_CHANGE_ROUTINE routine;
    void *context;
};

/***********************************************************************
 *           RegisterAppStateChangeNotification (twinapi.appcore.@)
 */
ULONG WINAPI RegisterAppStateChangeNotification( PAPPSTATE_CHANGE_ROUTINE routine, void *context, PAPPSTATE_REGISTRATION *reg )
{
    struct app_state_registration *registration;

    FIXME( "routine %p, context %p, reg %p - semi-stub, never notifies suspend/resume.\n", routine, context, reg );

    if (!(registration = calloc( 1, sizeof(*registration) ))) return ERROR_OUTOFMEMORY;
    registration->routine = routine;
    registration->context = context;

    *reg = (PAPPSTATE_REGISTRATION)registration;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *           UnregisterAppConstrainedChangeNotification (twinapi.appcore.@)
 */
void WINAPI UnregisterAppConstrainedChangeNotification( PAPPCONSTRAIN_REGISTRATION reg )
{
    FIXME( "reg %p - stub.\n", reg );
}

/***********************************************************************
 *           UnregisterAppStateChangeNotification (twinapi.appcore.@)
 */
void WINAPI UnregisterAppStateChangeNotification( PAPPSTATE_REGISTRATION reg )
{
    FIXME( "reg %p - stub.\n", reg );
    free( reg );
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "class %s, factory %p.\n", debugstr_hstring(classid), factory );

    *factory = NULL;

    if (!wcscmp( buffer, RuntimeClass_Windows_Security_ExchangeActiveSyncProvisioning_EasClientDeviceInformation ))
        IActivationFactory_QueryInterface( client_device_information_factory, &IID_IActivationFactory, (void **)factory );
    else if (!wcscmp( buffer, RuntimeClass_Windows_System_Profile_AnalyticsInfo ))
        IActivationFactory_QueryInterface( analytics_info_factory, &IID_IActivationFactory, (void **)factory );
    else if (!wcscmp( buffer, RuntimeClass_Windows_System_UserProfile_AdvertisingManager ))
        IActivationFactory_QueryInterface( advertising_manager_factory, &IID_IActivationFactory, (void **)factory );
    else if (!wcscmp( buffer, RuntimeClass_Windows_UI_ViewManagement_ApplicationView ))
        IActivationFactory_QueryInterface( application_view_factory, &IID_IActivationFactory, (void **)factory );
    else if (!wcscmp( buffer, RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager ))
        IActivationFactory_QueryInterface( data_transfer_manager_statics_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}
