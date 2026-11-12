/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _GPU_PRESENT_H
#define _GPU_PRESENT_H


/*!	GPU buffer presentation through the accelerant (Renga, roadmap F-47;
	request R-1 of the Quasar NVIDIA driver, Techniix/nvidia-haiku,
	docs/RENGA-REQUESTS.md).

	PROVISIONAL. This is the table the Quasar driver proposed, with the
	names and the header's home decided (F-58); app_server does not use it
	yet. The app_server side (GPU buffers in overlay bitmaps, a flip queue,
	direct scanout) comes with the GPU stage (F-47), after the open points
	on real hardware are settled. Until then the layout may change; every
	change bumps B_GPU_PRESENT_VERSION, and an accelerant fills in the
	version it implements, so app_server can refuse a table it does not
	know.

	An accelerant offers the table through get_accelerant_hook():
		gpu_present_hooks* hooks
			= (gpu_present_hooks*)get_accelerant_hook(B_GPU_PRESENT_HOOKS,
				NULL);
	It is called by app_server only (the accelerant's owner).

	Buffers are file descriptors (B_GPU_BUFFER_FD): a driver-specific
	shareable descriptor, an nvidiactl memory descriptor for Quasar or a
	dma-buf descriptor for LinuxKPI DRM drivers. An application hands its
	descriptor to app_server with BPrivate::app_server_send_fd()
	(headers/private/app/ServerFdTransfer.h, SCM_RIGHTS on a local socket
	next to the app_server link), and refers to it in app_server messages
	by the token that returns.

	Events (flip completion, vblank) are queued by the accelerant;
	event_fd() is readable while any are queued, for select(), poll() and
	epoll in app_server's own thread.
*/


#include <GraphicsDefs.h>
#include <OS.h>


// get_accelerant_hook() feature code ('RGA', vendor neutral).
#define B_GPU_PRESENT_HOOKS		0x52474100
#define B_GPU_PRESENT_VERSION	1


// gpu_buffer_desc::handle_type
enum {
	B_GPU_BUFFER_FD = 1		// handle is a file descriptor
};

// gpu_present_caps::flags
enum {
	B_GPU_PRESENT_VRR			= 0x01,	// variable refresh rate
	B_GPU_PRESENT_ASYNC_FLIP	= 0x02,	// flips without waiting for vblank
	B_GPU_PRESENT_SCALING		= 0x04	// planes scale source to destination
};

// gpu_flip_params::flags
enum {
	B_GPU_FLIP_VSYNC	= 0x01,
	B_GPU_FLIP_ASYNC	= 0x02,
	B_GPU_FLIP_VRR		= 0x04
};

// gpu_present_event::type
enum {
	B_GPU_EVENT_FLIP_COMPLETE	= 1,
	B_GPU_EVENT_VBLANK			= 2
};


typedef struct {
	int32		x;
	int32		y;
	uint32		width;
	uint32		height;
} gpu_rect;


typedef struct {
	uint32		handle_type;		// B_GPU_BUFFER_FD
	int			handle;				// driver-specific shareable descriptor
	uint32		width;
	uint32		height;
	uint32		bytes_per_row;		// 0 if not linear
	color_space	format;
	uint64		modifier;			// driver tiling/layout, 0 = linear
	uint64		offset;				// of the image inside the object
} gpu_buffer_desc;


typedef struct {
	uint32		planes_per_head;	// 1 = primary only
	uint32		flags;				// B_GPU_PRESENT_*
	uint32		max_width;
	uint32		max_height;
} gpu_present_caps;


typedef struct {
	gpu_rect	source;				// buffer pixels
	gpu_rect	destination;		// on screen; ignored for the primary plane
	uint32		flags;				// B_GPU_FLIP_*
	int			wait_fd;			// -1, or a descriptor that becomes
									// readable when rendering is complete
	uint64		user_data;			// returned in the completion event
} gpu_flip_params;


typedef struct {
	uint32		type;				// B_GPU_EVENT_*
	uint32		head;
	uint32		plane;
	uint64		user_data;
	bigtime_t	time;				// system_time() of the event
	uint64		sequence;			// vblank count
} gpu_present_event;


typedef struct {
	uint32		version;			// B_GPU_PRESENT_VERSION implemented
	status_t	(*get_caps)(uint32 head, gpu_present_caps* caps);
	status_t	(*import_buffer)(const gpu_buffer_desc* desc, uint32* _id);
	status_t	(*release_buffer)(uint32 id);
	status_t	(*flip)(uint32 head, uint32 plane, uint32 id,
					const gpu_flip_params* params);
	status_t	(*disable_plane)(uint32 head, uint32 plane);
	status_t	(*request_vblank)(uint32 head, bool repeat);
	int			(*event_fd)(void);
	status_t	(*next_event)(gpu_present_event* event);
	// R-2: the current scanout buffer of a head, as a new descriptor.
	status_t	(*export_scanout)(uint32 head, gpu_buffer_desc* desc);
} gpu_present_hooks;


#endif	// _GPU_PRESENT_H
