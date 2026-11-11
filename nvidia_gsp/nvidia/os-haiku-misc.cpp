/*
 * OS-interface functions that used to be panic() stubs in stubs.c.
 *
 * NVIDIA's Resource Manager (nv-kernel.o) calls a few hundred functions the
 * OS port has to supply. The port started with every one of them as a stub
 * that calls panic(), and replaced the ones the bring-up reached. The rest
 * stayed fatal: with this driver owning the display the kernel debugger is
 * not visible, so reaching one looks like a hard freeze of the machine.
 *
 * This file gives the remaining stubs that RM can plausibly reach on a
 * desktop GPU their real prototypes and one of three behaviours:
 *
 *  - implemented, where the function is small and its contract is clear
 *    (logging level, stack dump on an RM assertion, version, random bytes,
 *    PCI lookup, driver settings);
 *  - a no-op, where Linux's own implementation is a no-op on x86 without an
 *    IOMMU (cache flushes, SoC interrupt control);
 *  - a logged refusal (NV_ERR_NOT_SUPPORTED, NV_FALSE or NULL), where the
 *    feature does not exist on Haiku yet (dma-buf, pinned user pages, NUMA,
 *    runtime power management). RM handles these errors: the operation that
 *    needed the feature fails and the system keeps running. Each refusal is
 *    logged once with the caller's return addresses, so the syslog names the
 *    code path that wanted it.
 *
 * What is left in stubs.c is either unreachable on a single desktop GPU
 * (NVLink, NVSwitch, IBM NPU, confidential-computing crypto) or has no safe
 * value to return.
 *
 * The prototypes come from nv.h and os-interface.h. Because those headers
 * declare the functions with C linkage, a definition here whose parameters
 * differ from the declaration does not compile, which is the check that the
 * old "int name(void)" stubs could not give.
 */

#include "nv-include.h"
extern "C" {
#include <os-interface.h>
}

#include <stdio.h>
#include <string.h>

#include <OS.h>
#include <KernelExport.h>
#include <driver_settings.h>
#include <arch/debug.h>

#include "Driver.h"
#include "Device.h"
#include "os-haiku-misc.h"


// #pragma mark - data symbols


/* These three are variables in NVIDIA's OS interface, not functions. stubs.c
 * defined them as functions, so RM read the first bytes of machine code as
 * their value:
 *
 *  - os_dma_buf_enabled (NvBool) read as true, so RM reported dma-buf support
 *    that the port does not have;
 *  - nvidia_caps_root (a pointer) read as non-NULL garbage;
 *  - nv_parms (a NULL-terminated table) was walked as a table by
 *    rm_parse_option_string(), which is only called with a registry string
 *    and so was never reached, but would have read code as pointers.
 */
extern "C" {

NvBool os_dma_buf_enabled = NV_FALSE;

nv_cap_t *nvidia_caps_root = NULL;

/* Linux lists its module parameters here so a "RegistryDwords" entry can set
 * one by name. This port has no such parameters: every key in the
 * registry_dwords driver setting goes straight to RM's registry, which is
 * what an empty table makes rm_parse_option_string() do. */
nv_parm_t nv_parms[] = {
	{NULL, NULL}
};

}


// #pragma mark - logging


/* Same encoding as Linux's cur_debuglevel: bits 4-5 hold the lowest
 * NV_DBG_* level that nv_printf() prints. 0xffffffff, the default on Linux
 * too, prints warnings and errors only. */
static NvU32 sDebugLevel = 0xffffffff;
static bool sVerbose = false;


bool
nv_haiku_should_log(NvU32 level)
{
	if (sVerbose)
		return true;
	return level >= ((sDebugLevel >> 4) & 0x3);
}


bool
nv_haiku_verbose()
{
	return sVerbose;
}


/* Everything from here on is declared with C linkage in nv.h and
 * os-interface.h. Defining it inside extern "C" makes a definition whose
 * parameters differ from the declaration a compile error, instead of a
 * silently separate C++ overload that RM would not find at link time. */
extern "C" {

void NV_API_CALL os_dbg_set_level(NvU32 newLevel)
{
	sDebugLevel = newLevel;
}


/* Print the kernel return addresses of the current thread. Unlike panic()
 * this leaves the system running. The addresses can be resolved against the
 * loaded image list (the "images" command of the kernel debugger, or the
 * image list in a Renga crash report). */
static void
print_stack_trace(const char *prefix)
{
	addr_t addresses[16];
	int32 count = arch_get_stack_trace(addresses, 16, 0, 1,
		STACK_TRACE_KERNEL);
	char line[16 * 20 + 1];
	size_t length = 0;
	line[0] = '\0';
	for (int32 i = 0; i < count; i++) {
		length += snprintf(line + length, sizeof(line) - length, " %#" B_PRIxADDR,
			addresses[i]);
		if (length >= sizeof(line))
			break;
	}
	dprintf("%s: return addresses:%s\n", prefix, line);
}


/* RM calls this from osAssertFailed(), that is, on every failed NV_ASSERT.
 * RM's assertions are diagnostics: on Linux this is dump_stack() and the
 * driver carries on. As a panic() stub, any failed assertion in RM stopped
 * the whole machine. */
void NV_API_CALL os_dump_stack(void)
{
	print_stack_trace("nvidia_gsp: RM assertion or stack dump request");
}


/* Log that RM wanted a feature this port lacks: once per feature, with the
 * call stack. */
static void
log_unsupported(const char *function, int32 *reported)
{
	if (atomic_get_and_set(reported, 1) != 0)
		return;
	dprintf("nvidia_gsp: %s is not supported on Haiku; the operation that "
		"needed it will fail (reported once)\n", function);
	print_stack_trace("nvidia_gsp");
}

#define UNSUPPORTED() \
	do { \
		static int32 sReported = 0; \
		log_unsupported(__func__, &sReported); \
	} while (0)


} // extern "C"


// #pragma mark - driver settings


/* Settings file: nvidia_gsp in a kernel/drivers settings directory, for
 * example /boot/home/config/settings/kernel/drivers/nvidia_gsp.
 *
 *   verbose true
 *       Print every RM message and the driver's own progress messages.
 *       The default prints warnings and errors.
 *   registry_dwords "Key1=1;Key2=0x10"
 *       RM registry values, in the format of the Linux driver's
 *       NVreg_RegistryDwords module parameter. Meant for debugging and for
 *       trying RM options without rebuilding the driver.
 *
 * Called from os_registry_init(), which RM calls once from rm_init_rm(),
 * before it reads any registry key.
 */
void
nv_haiku_load_settings()
{
	void *settings = load_driver_settings("nvidia_gsp");
	if (settings == NULL)
		return;

	sVerbose = get_driver_boolean_parameter(settings, "verbose", false, true);

	const char *dwords = get_driver_parameter(settings, "registry_dwords",
		NULL, NULL);
	if (dwords != NULL && dwords[0] != '\0') {
		dprintf("nvidia_gsp: registry_dwords: %s\n", dwords);
		rm_parse_option_string(NULL, dwords);
	}

	unload_driver_settings(settings);
}


extern "C" {

// #pragma mark - implemented


/* RM passes the result to GSP and into debug dumps. Haiku has no numeric
 * release triple in the kernel, so report the kernel's revision number as
 * the build and its build date as the version strings. */
NV_STATUS NV_API_CALL os_get_version_info(os_version_info *info)
{
	static system_info sSystemInfo;
	static int32 sLoaded = 0;
	if (atomic_get(&sLoaded) == 0) {
		get_system_info(&sSystemInfo);
		atomic_set(&sLoaded, 1);
	}

	info->os_major_version = 1;
	info->os_minor_version = 0;
	info->os_build_number = (NvU32)sSystemInfo.kernel_version;
	info->os_build_version_str = sSystemInfo.kernel_build_date;
	info->os_build_date_plus_str = sSystemInfo.kernel_build_time;
	return NV_OK;
}


static inline bool
cpu_has_rdrand()
{
	uint32 eax = 1, ebx = 0, ecx = 0, edx = 0;
	__asm__("cpuid" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));
	return (ecx & (1u << 30)) != 0;
}


static inline bool
rdrand64(uint64 *value)
{
	unsigned char ok;
	__asm__("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok));
	return ok != 0;
}


/* RM uses this for session nonces and identifiers. RDRAND is used when the
 * CPU has it (every CPU that can host a Turing or newer card in practice).
 * The fallback mixes the time and thread into a splitmix64 stream; it is not
 * cryptographically strong and says so in the log. */
NV_STATUS NV_API_CALL os_get_random_bytes(NvU8 *bytes, NvU16 numBytes)
{
	static int32 sHasRdrand = -1;
	if (sHasRdrand < 0)
		atomic_set(&sHasRdrand, cpu_has_rdrand() ? 1 : 0);

	static uint64 sState = 0;
	NvU16 done = 0;
	while (done < numBytes) {
		uint64 value = 0;
		bool ok = false;
		if (sHasRdrand == 1) {
			for (int attempt = 0; attempt < 10 && !ok; attempt++)
				ok = rdrand64(&value);
		}
		if (!ok) {
			static int32 sWarned = 0;
			if (atomic_get_and_set(&sWarned, 1) == 0) {
				dprintf("nvidia_gsp: no RDRAND; os_get_random_bytes() uses a "
					"weak generator\n");
			}
			uint64 z = (sState += 0x9e3779b97f4a7c15ull
				+ (uint64)system_time() + (uint64)find_thread(NULL));
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			value = z ^ (z >> 31);
		}
		NvU16 chunk = numBytes - done;
		if (chunk > sizeof(value))
			chunk = sizeof(value);
		memcpy(bytes + done, &value, chunk);
		done += chunk;
	}
	return NV_OK;
}


/* Used by the NV_ESC_ATTACH/WAIT style control-device ioctls to find a GPU
 * by PCI location. Returns NULL when there is none, which RM reports to the
 * caller as an invalid argument. */
nv_state_t* NV_API_CALL nv_get_adapter_state(NvU32 domain, NvU8 bus, NvU8 slot)
{
	uint32 count = NvHaikuDriver::Instance().DeviceCount();
	for (uint32 i = 0; i < count; i++) {
		nv_state_t *nv = NvHaikuDriver::Instance().DeviceAt(i)->Nv();
		if (nv->pci_info.domain == domain && nv->pci_info.bus == bus
			&& nv->pci_info.slot == slot)
			return nv;
	}
	return NULL;
}


// #pragma mark - no-ops, as on Linux x86


/* Linux implements these only for ARM, where device memory is not coherent
 * with the CPU caches. On x86 they do nothing. */
void NV_API_CALL nv_dma_cache_invalidate(nv_dma_device_t *, void *)
{
}

void NV_API_CALL nv_flush_coherent_cpu_cache_range(nv_state_t *, NvU64, NvU64)
{
}

NV_STATUS NV_API_CALL os_flush_user_cache(void)
{
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_control_soc_irqs(nv_state_t *, NvBool)
{
}

NvBool NV_API_CALL os_pci_remove_supported(void)
{
	return NV_FALSE;
}


// #pragma mark - not supported: dma-buf and DMA remapping


NV_STATUS NV_API_CALL nv_dma_import_dma_buf(nv_dma_device_t *,
	struct dma_buf *, NvU32 *, struct sg_table **, nv_dma_buf_t **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_dma_import_from_fd(nv_dma_device_t *, NvS32, NvU32 *,
	struct sg_table **, nv_dma_buf_t **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_dma_import_sgt(nv_dma_device_t *, struct sg_table *,
	struct drm_gem_object *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_dma_release_dma_buf(nv_dma_buf_t *)
{
	UNSUPPORTED();
}

void NV_API_CALL nv_dma_release_sgt(struct sg_table *, struct drm_gem_object *)
{
	UNSUPPORTED();
}

/* RM calls these only when the GPU has an IOVA space, that is, when
 * nv_requires_dma_remap() says so. It returns false here: Haiku has no
 * IOMMU support, and bus addresses equal physical addresses. */
NV_STATUS NV_API_CALL nv_dma_map_alloc(nv_dma_device_t *, NvU64, NvU64 *,
	NvBool, void **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_dma_unmap_alloc(nv_dma_device_t *, NvU64, NvU64 *,
	void **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_dma_map_peer(nv_dma_device_t *, nv_dma_device_t *,
	NvU8, NvU64, NvU64 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_dma_unmap_peer(nv_dma_device_t *, NvU64, NvU64)
{
	UNSUPPORTED();
}

NV_STATUS NV_API_CALL nv_dma_map_mmio(nv_dma_device_t *, NvU64, NvU64 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_dma_unmap_mmio(nv_dma_device_t *, NvU64, NvU64)
{
	UNSUPPORTED();
}


// #pragma mark - not supported: importing memory RM did not allocate


/* Pinned user pages back NV01_MEMORY_SYSTEM_OS_DESCRIPTOR allocations, which
 * is what VK_EXT_external_memory_host needs. Haiku can wire user memory with
 * lock_memory_etc(), but only for the lifetime of the area and the team: a
 * long-term pin that RM releases when the client's file is closed needs
 * support the kernel does not have yet (docs/RENGA-REQUESTS.md, R-5). Until
 * then the allocation fails cleanly and NVK does not offer the extension. */
NV_STATUS NV_API_CALL os_lock_user_pages(void *, NvU64, void **, NvU32)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_unlock_user_pages(NvU64, void *, NvU32)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_lookup_user_io_memory(void *, NvU64, NvU64 **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_register_user_pages(nv_state_t *, NvU64, NvU64 *,
	void *, void **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_unregister_user_pages(nv_state_t *, NvU64, void **,
	void **)
{
	UNSUPPORTED();
}

NV_STATUS NV_API_CALL nv_register_phys_pages(nv_state_t *, NvU64 *, NvU64,
	NvU32, void **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_unregister_phys_pages(nv_state_t *, void *)
{
	UNSUPPORTED();
}

NV_STATUS NV_API_CALL nv_alias_pages(nv_state_t *, NvU32, NvU64, NvU32, NvU32,
	NvU64, NvU64 *, NvBool, void **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_num_phys_pages(void *, NvU32 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_phys_pages(void *, void *, NvU32 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}


// #pragma mark - not supported: NUMA, page offlining, files, virtualisation


NV_STATUS NV_API_CALL os_get_numa_node_memory_usage(NvS32, NvU64 *, NvU64 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_numa_add_gpu_memory(void *, NvU64, NvU64, NvU32 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_numa_remove_gpu_memory(void *, NvU64, NvU64, NvU32)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_numa_memblock_size(NvU64 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_alloc_pages_node(NvS32, NvU32, NvU32, NvU64 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_offline_page_at_address(NvU64)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_get_page(NvU64)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_put_page(NvU64)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_get_smbios_header(NvU64 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_egm_info(nv_state_t *, NvU64 *, NvU64 *, NvS32 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

/* RM saves video memory to a temporary file across suspend when asked to
 * preserve allocations. Suspend is not supported yet (Renga F-37). */
NV_STATUS NV_API_CALL os_open_temporary_file(void **)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL os_close_file(void *)
{
	UNSUPPORTED();
}

NV_STATUS NV_API_CALL os_write_file(void *, NvU8 *, NvU64, NvU64)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_read_file(void *, NvU8 *, NvU64, NvU64)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_enable_pci_req_atomics(void *,
	enum os_pci_req_atomics_type)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_call_vgpu_vfio(void *, NvU32)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_inject_vgx_msi(NvU16, NvU64, NvU32)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_schedule_uvm_drain_p2p(NvU8 *)
{
	UNSUPPORTED();
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_schedule_uvm_resume_p2p(NvU8 *)
{
	UNSUPPORTED();
}

NvBool NV_API_CALL nv_match_gpu_os_info(nv_state_t *, void *)
{
	UNSUPPORTED();
	return NV_FALSE;
}


// #pragma mark - not supported: runtime power management


/* These report that the platform has no runtime power management, which is
 * what Linux's versions return on a kernel without CONFIG_PM. RM then keeps
 * the GPU powered and does not start its idle (GC6/D3) state machine, so the
 * mmap-revocation functions that machine needs stay unreachable; those are
 * still panic stubs in stubs.c on purpose. Real GPU power management is
 * tracked in docs/RENGA-STATUS.md. */
NvBool NV_API_CALL nv_dynamic_power_available(nv_state_t *)
{
	return NV_FALSE;
}

NvBool NV_API_CALL nv_platform_supports_s0ix(void)
{
	return NV_FALSE;
}

NvBool NV_API_CALL nv_s2idle_pm_configured(void)
{
	return NV_FALSE;
}

void NV_API_CALL nv_allow_runtime_suspend(nv_state_t *)
{
}

void NV_API_CALL nv_disallow_runtime_suspend(nv_state_t *)
{
}

void NV_API_CALL nv_idle_holdoff(nv_state_t *)
{
}

NV_STATUS NV_API_CALL nv_indicate_idle(nv_state_t *)
{
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_indicate_not_idle(nv_state_t *)
{
	return NV_OK;
}


// #pragma mark - I/O ports


/* Legacy I/O port access (VGA registers). RM uses it on some paths for
 * cards that expose an I/O BAR. */
NvU8 NV_API_CALL os_io_read_byte(NvU32 port)
{
	NvU8 value;
	__asm__ volatile("inb %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

NvU16 NV_API_CALL os_io_read_word(NvU32 port)
{
	NvU16 value;
	__asm__ volatile("inw %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

NvU32 NV_API_CALL os_io_read_dword(NvU32 port)
{
	NvU32 value;
	__asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

void NV_API_CALL os_io_write_byte(NvU32 port, NvU8 value)
{
	__asm__ volatile("outb %0, %w1" : : "a"(value), "Nd"(port));
}

void NV_API_CALL os_io_write_word(NvU32 port, NvU16 value)
{
	__asm__ volatile("outw %0, %w1" : : "a"(value), "Nd"(port));
}

void NV_API_CALL os_io_write_dword(NvU32 port, NvU32 value)
{
	__asm__ volatile("outl %0, %w1" : : "a"(value), "Nd"(port));
}

} // extern "C"
