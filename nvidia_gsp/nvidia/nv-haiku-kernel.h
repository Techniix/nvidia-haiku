#pragma once

#include <module.h>

extern "C" {
#include "nvtypes.h"
#include "nv-gpu-info.h"
}
#include "nvUnixVersion.h"


// Versioned so nvidia_gsp_modeset only binds to an nvidia_gsp built from the
// same RM release; follows the submodule's NV_VERSION_STRING.
#define NV_HAIKU_MODULE_NAME "drivers/dev/graphics/nvidia_gsp/v" NV_VERSION_STRING


/*
 * Callback functions from the RM OS interface layer into the NVKMS OS interface
 * layer.
 *
 * These functions should be called without the RM lock held, using the kernel's
 * native calling convention.
 */
typedef struct {
    /*
     * Suspend & resume callbacks.  Note that these are called once per GPU.
     */
    void (*suspend)(NvU32 gpu_id);
    void (*resume)(NvU32 gpu_id);
} nvidia_modeset_callbacks_t;


struct nvidia_module_info {
	module_info base;

    struct {
        /* Availability of write combining support for video memory */
        NvBool allow_write_combining;
    } system_info;

    /*
     * Enumerate list of gpus probed by nvidia driver.
     *
     * gpu_info is an array of NVIDIA_MAX_GPUS elements. The number of GPUs
     * in the system is returned.
     */
    NvU32 (*enumerate_gpus)(nv_gpu_info_t *gpu_info);

    /*
     * {open,close}_gpu() raise and lower the reference count of the
     * specified GPU.  This is equivalent to opening and closing a
     * /dev/nvidiaN device file from user-space.
     */
    int (*open_gpu)(NvU32 gpu_id);
    void (*close_gpu)(NvU32 gpu_id);

    void (*op)(void *ops_cmd);

    int (*set_callbacks)(const nvidia_modeset_callbacks_t *cb);
};
