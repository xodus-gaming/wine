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

/* A registration this module never raises anything against.
 *
 * Nothing here is read; it exists so the caller has a distinct pointer to hold
 * and to hand back when it unregisters. */
struct notification_registration
{
    void *routine;
    void *context;
};

/***********************************************************************
 *           RegisterAppConstrainedChangeNotification (twinapi.appcore.@)
 */
ULONG WINAPI RegisterAppConstrainedChangeNotification( PAPPCONSTRAIN_CHANGE_ROUTINE routine, void *context, PAPPCONSTRAIN_REGISTRATION *reg )
{
    struct notification_registration *impl;

    FIXME( "routine %p, context %p, reg %p: accepted, no constraint change will be raised.\n",
           routine, context, reg );

    if (!reg) return ERROR_INVALID_PARAMETER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return ERROR_OUTOFMEMORY;
    impl->routine = routine;
    impl->context = context;
    *reg = (PAPPCONSTRAIN_REGISTRATION)impl;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *           RegisterAppStateChangeNotification (twinapi.appcore.@)
 */
ULONG WINAPI RegisterAppStateChangeNotification( PAPPSTATE_CHANGE_ROUTINE routine, void *context, PAPPSTATE_REGISTRATION *reg )
{
    struct notification_registration *impl;

    FIXME( "routine %p, context %p, reg %p: accepted, no state change will be raised.\n",
           routine, context, reg );

    if (!reg) return ERROR_INVALID_PARAMETER;

    /* Refusing left *reg untouched, and a caller reads it back regardless of
     * the result: an earlier title was seen unregistering 0x6572757463, which
     * is leftover text off its own stack. Balatro takes the refusal itself as
     * fatal and exits cleanly before opening a window, which looks exactly
     * like a title that simply does not run.
     *
     * The app is never suspended or resumed here, so accepting costs nothing:
     * the routine would never be called either way. */
    if (!(impl = calloc( 1, sizeof(*impl) ))) return ERROR_OUTOFMEMORY;
    impl->routine = routine;
    impl->context = context;
    *reg = (PAPPSTATE_REGISTRATION)impl;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *           UnregisterAppConstrainedChangeNotification (twinapi.appcore.@)
 */
void WINAPI UnregisterAppConstrainedChangeNotification( PAPPCONSTRAIN_REGISTRATION reg )
{
    TRACE( "reg %p.\n", reg );
    free( reg );
}

/***********************************************************************
 *           UnregisterAppStateChangeNotification (twinapi.appcore.@)
 */
void WINAPI UnregisterAppStateChangeNotification( PAPPSTATE_REGISTRATION reg )
{
    TRACE( "reg %p.\n", reg );
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
