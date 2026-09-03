/* cps-ncl.h - CyberPower proprietary NCL bank support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef CPS_NCL_H
#define CPS_NCL_H

#include "libhid.h"

void cps_ncl_initinfo(HIDDevice_t *device);
void cps_ncl_updateinfo(void);
int cps_ncl_instcmd(const char *cmdname, const char *extradata);

#endif /* CPS_NCL_H */
