/* WinRT Windows.Gaming.Input provider helpers
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

#ifndef __WINE_WINDOWS_GAMING_INPUT_PROVIDER_PRIVATE_H
#define __WINE_WINDOWS_GAMING_INPUT_PROVIDER_PRIVATE_H

static inline HRESULT provider_create_nonroamable_id(const WCHAR *path, UINT16 vid, UINT16 pid, HSTRING *value)
{
    const WCHAR *xi, *instance;
    WCHAR buffer[1024];
    int len;

    if (vid == 0x28de && pid == 0x11ff) return E_NOTIMPL;

    for (xi = path; *xi; ++xi)
    {
        if (xi[0] != L'&' || !xi[1]) continue;
        if ((xi[1] != L'x' && xi[1] != L'X') || !xi[2]) continue;
        if ((xi[2] != L'i' && xi[2] != L'I') || xi[3] != L'_') continue;
        break;
    }
    if (!*xi || !(instance = wcschr(xi + 4, L'#')) || !instance[1]) return E_NOTIMPL;

    ++instance;
    len = swprintf(buffer, ARRAY_SIZE(buffer), L"{wgi/nrid/:wine-%04X&%04X&%ls}", vid, pid, instance);
    if (len < 0 || len >= (int)ARRAY_SIZE(buffer)) return E_BOUNDS;
    return WindowsCreateString(buffer, len, value);
}

#endif /* __WINE_WINDOWS_GAMING_INPUT_PROVIDER_PRIVATE_H */
