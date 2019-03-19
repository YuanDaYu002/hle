#ifndef _NETDEV_H_
#define _NETDEV_H_
//==========================================================================
//
//      include/netdev.h
//
//      Network device description
//
//==========================================================================
// ####ECOSGPLCOPYRIGHTBEGIN####
// -------------------------------------------
// This file is part of eCos, the Embedded Configurable Operating System.
// Copyright (C) 1998, 1999, 2000, 2001, 2002 Free Software Foundation, Inc.
//
// eCos is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 or (at your option) any later
// version.
//
// eCos is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.
//
// You should have received a copy of the GNU General Public License
// along with eCos; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
// As a special exception, if other files instantiate templates or use
// macros or inline functions from this file, or you compile this file
// and link it with other works to produce a work based on this file,
// this file does not by itself cause the resulting work to be covered by
// the GNU General Public License. However the source code for this file
// must still be made available in accordance with section (3) of the GNU
// General Public License v2.
//
// This exception does not invalidate any other reasons why a work based
// on this file might be covered by the GNU General Public License.
// -------------------------------------------
// ####ECOSGPLCOPYRIGHTEND####
//==========================================================================
//#####DESCRIPTIONBEGIN####
//
// Author(s):    gthomas
// Contributors: gthomas
// Date:         2000-01-10
// Purpose:
// Description:
//
//
//####DESCRIPTIONEND####
//
//==========================================================================

//#include <cyg/hal/hal_tables.h>

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

// Network device support

typedef struct cyg_netdevtab_entry {
    const char        *name;
    bool             (*init)(struct cyg_netdevtab_entry *tab);
    void              *device_instance;  // Local data, instance specific
    unsigned long     status;
} __attribute__((aligned(4))) cyg_netdevtab_entry_t;

#define CYG_NETDEVTAB_STATUS_AVAIL   0x0001

extern cyg_netdevtab_entry_t __NETDEVTAB__[], __NETDEVTAB_END__;

#define NETDEVTAB_ENTRY(_l,_name,_init,_instance)  \
static bool _init(struct cyg_netdevtab_entry *tab);                  \
cyg_netdevtab_entry_t _l  = {             \
   _name,                                                            \
   _init,                                                            \
   _instance                                                         \
};

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif // _NETDEV_H_
