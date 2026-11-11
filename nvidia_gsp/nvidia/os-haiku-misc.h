#pragma once

#include "nv-include.h"

/* True when a message of this NV_DBG_* level should reach the syslog. */
bool nv_haiku_should_log(NvU32 level);

/* True when the "verbose" driver setting is on: the driver's own progress
 * messages are printed too. */
bool nv_haiku_verbose();

/* Read the nvidia_gsp driver settings file (see os-haiku-misc.cpp). */
void nv_haiku_load_settings();
