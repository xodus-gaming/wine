/* WinRT Windows.Gaming.Input provider identity helper
 *
 * Copyright 2026 volcmen
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

#include <stdarg.h>
#include <wchar.h>

#include "windef.h"
#include "winerror.h"
#include "winstring.h"

#include "../provider_private.h"

HRESULT provider_create_nonroamable_id( const WCHAR *path, UINT16 vid, UINT16 pid, HSTRING *value )
{
    const WCHAR *separator, *xi, *instance;
    WCHAR buffer[1024];
    int len;

    if (vid == 0x28de && pid == 0x11ff) return E_NOTIMPL;

    if (!(separator = wcschr( path, L'#' )) || !(instance = wcschr( separator + 1, L'#' )) || !instance[1])
        return E_NOTIMPL;
    if (instance - path < 6) return E_NOTIMPL;

    xi = instance - 6;
    if (xi[0] != L'&' || (xi[1] != L'x' && xi[1] != L'X') ||
        (xi[2] != L'i' && xi[2] != L'I') || xi[3] != L'_' ||
        xi[4] < L'0' || xi[4] > L'9' || xi[5] < L'0' || xi[5] > L'9')
        return E_NOTIMPL;

    ++instance;
    len = swprintf( buffer, ARRAY_SIZE( buffer ), L"{wgi/nrid/:wine-%04X&%04X&%ls}", vid, pid, instance );
    if (len < 0 || len >= (int)ARRAY_SIZE( buffer )) return E_BOUNDS;
    return WindowsCreateString( buffer, len, value );
}
