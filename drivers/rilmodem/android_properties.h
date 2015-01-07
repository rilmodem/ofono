/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Canonical Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#if defined(WITH_HYBRIS_ANDROID_PROPERTIES)

#include <hybris/properties/properties.h>

#define HAVE_ANDROID_PROP 1

#else /* !WITH_HYBRIS_ANDROID_PROPERTIES */

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92

#define property_set(key, value) -1
#define property_get(key, value, default_value) -1
#define property_list(propfn, cookie) -1

#if defined(HAVE_ANDROID_PROP)
#undef HAVE_ANDROID_PROP
#endif

#endif /* !WITH_HYBRIS_ANDROID_PROPERTIES */
