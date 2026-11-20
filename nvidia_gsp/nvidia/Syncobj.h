#pragma once

/*
 * Renga's DRM sync object module (R-18, Renga 1424ffb4), as seen by the
 * kernel driver.
 *
 * <syncobj_module.h> is installed with Renga's development headers from
 * the first image built after 1424ffb4. Older images (143b8c30, the one the
 * first fresh-install builds ran on) do not have it, and Quasar must keep
 * building there until a newer image is out, so the header is optional for
 * now: without it QUASAR_HAVE_SYNCOBJ is 0, the driver never asks for the
 * module, and the syncobj ioctls are not answered, exactly as when the
 * module is missing at run time. docs/RENGA-STATUS.md lists this under
 * transitional code; the check goes away, and the header becomes required,
 * once renga-setup.sh's image check asks for an image that carries it.
 */

#if __has_include(<syncobj_module.h>)
#	include <syncobj_module.h>
#	define QUASAR_HAVE_SYNCOBJ 1
#else
#	define QUASAR_HAVE_SYNCOBJ 0
#endif
