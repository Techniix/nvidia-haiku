/*
 * GPU presentation through the accelerant: buffer import, page flips with
 * completion events, vblank events and scanout export.
 *
 * PROVISIONAL. This is the interface proposed in Quasar's
 * docs/RENGA-REQUESTS.md (R-1) for Renga's app_server (roadmap F-35/F-47).
 * It is meant to become vendor-neutral and to live in Renga's headers, so
 * that the LinuxKPI DRM drivers implement the same table. Until the Renga
 * side accepts it, the names, the feature code and the structures may change,
 * and B_GPU_PRESENT_VERSION will be bumped when they do.
 *
 * app_server gets the table with
 *     get_accelerant_hook(B_GPU_PRESENT_HOOKS, NULL)
 * which returns a const gpu_present_hooks*, or NULL when the driver has none.
 * Calls are made by the accelerant's owner (app_server), from any thread.
 */
#ifndef _GPU_PRESENT_H
#define _GPU_PRESENT_H

#include <GraphicsDefs.h>
#include <OS.h>

#ifdef __cplusplus
extern "C" {
#endif

#define B_GPU_PRESENT_HOOKS		0x52474100	/* 'RGA\0' */
#define B_GPU_PRESENT_VERSION	1

/* gpu_buffer_desc::handle_type */
enum {
	B_GPU_BUFFER_FD = 1			/* a file descriptor the driver can import */
};

/* gpu_buffer_desc::modifier: 0 is linear (pitch) memory. Other values are the
 * driver's own layout codes; for Quasar they are the DRM format modifiers of
 * NVIDIA block-linear memory, as NVK reports them. */
#define B_GPU_MODIFIER_LINEAR	0

typedef struct {
	uint32		handle_type;
	int32		handle;
	uint32		width;
	uint32		height;
	uint32		bytes_per_row;	/* pitch for linear memory */
	color_space	format;			/* B_RGB32, B_RGBA32 */
	uint64		modifier;
	uint64		offset;			/* of the image in the memory object */
	uint64		size;			/* of the memory object, bytes */
} gpu_buffer_desc;

/* gpu_present_caps::flags */
enum {
	B_GPU_PRESENT_CAN_VRR		= 1 << 0,
	B_GPU_PRESENT_CAN_TEAR		= 1 << 1,	/* flips not tied to vblank */
	B_GPU_PRESENT_CAN_SCALE		= 1 << 2,
	B_GPU_PRESENT_CAN_POSITION	= 1 << 3	/* overlay planes in a window */
};

typedef struct {
	uint32		planes;			/* per head; plane 0 is the primary */
	uint32		flags;
	uint32		max_width;
	uint32		max_height;
} gpu_present_caps;

/* gpu_flip_params::flags */
enum {
	B_GPU_FLIP_ASYNC	= 1 << 0,	/* do not wait for vblank (tearing) */
	B_GPU_FLIP_VRR		= 1 << 1	/* allow variable refresh */
};

/* A rectangle in pixels. Plain C and integer, unlike BRect, so drivers in C
 * can implement the table and there is no off-by-one between inclusive and
 * exclusive edges. width or height 0: the whole buffer (source) or the
 * buffer's size at the origin (destination). */
typedef struct {
	int32		x;
	int32		y;
	uint32		width;
	uint32		height;
} gpu_rect;

typedef struct {
	gpu_rect	source;			/* buffer pixels; must start at 0,0 for now */
	gpu_rect	destination;	/* screen pixels; ignored for plane 0 */
	uint32		flags;
	int32		wait_fd;		/* reserved, must be -1 */
	uint64		user_data;		/* returned in B_GPU_EVENT_FLIP_COMPLETE */
} gpu_flip_params;

enum {
	B_GPU_EVENT_FLIP_COMPLETE	= 1,
	B_GPU_EVENT_VBLANK			= 2
};

typedef struct {
	uint32		type;
	uint32		head;
	uint32		plane;
	uint32		_reserved;
	uint64		user_data;
	bigtime_t	time;			/* system_time() when the driver saw it */
	uint64		sequence;
} gpu_present_event;

typedef struct {
	uint32		version;		/* B_GPU_PRESENT_VERSION */
	uint32		head_count;

	status_t	(*get_caps)(uint32 head, gpu_present_caps* caps);

	/* The descriptor stays owned by the caller. */
	status_t	(*import_buffer)(const gpu_buffer_desc* desc, uint32* _id);
	status_t	(*release_buffer)(uint32 id);

	/* Queue a flip. Completion is reported as a B_GPU_EVENT_FLIP_COMPLETE
	 * with the params' user_data. B_BUSY: a flip on this plane is still
	 * pending. */
	status_t	(*flip)(uint32 head, uint32 plane, uint32 id,
					const gpu_flip_params* params);
	status_t	(*disable_plane)(uint32 head, uint32 plane);

	status_t	(*request_vblank)(uint32 head);

	/* Readable while events are queued: select(), poll() and epoll. */
	int32		(*event_fd)(void);
	/* B_WOULD_BLOCK when no event is queued. */
	status_t	(*next_event)(gpu_present_event* event);

	/* A new descriptor for the memory now shown on a head's primary plane.
	 * The caller closes desc->handle. For privileged capture (R-2). */
	status_t	(*export_scanout)(uint32 head, gpu_buffer_desc* desc);
} gpu_present_hooks;

#ifdef __cplusplus
}
#endif

#endif	/* _GPU_PRESENT_H */
