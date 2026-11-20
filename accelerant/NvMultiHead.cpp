/*
 * Several displays on one GPU: Renga's <add-ons/graphics/MultiHead.h> on
 * NVKMS. See NvMultiHead.h for the numbering and what is not handled yet.
 *
 * Status: written against nvkms-api.h and syntax-checked for Haiku; not run.
 * app_server does not call the table yet (QUASAR.md 2026-11-20).
 */

#include "NvMultiHead.h"

#if QUASAR_HAVE_MULTI_HEAD

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <ErrorUtils.h>

#include "NvModes.h"

#include "common/edid.h"


NvMultiHead::NvMultiHead(const Context &context):
	fContext(context)
{
	NVDpyIdList validDpys;
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = fContext.kmsDev->Get();
		params.request.dispHandle = fContext.disp;
		CheckErrno(fContext.kms->Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params)));
		validDpys = params.reply.validDpys;
	}

	// Output 0 first, then the others in NVKMS's order.
	auto add = [&](NVDpyId dpy) {
		auto out = std::make_unique<Output>();
		out->dpy = dpy;
		NvKmsQueryDpyStaticDataParams params {};
		params.request.deviceHandle = fContext.kmsDev->Get();
		params.request.dispHandle = fContext.disp;
		params.request.dpyId = dpy;
		if (fContext.kms->Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &params,
				sizeof(params)) >= 0)
			out->headMask = params.reply.headMask;
		fOutputs.push_back(std::move(out));
	};
	add(fContext.classicDpy);
	fOutputs[0]->hwHead = fContext.classicHwHead;
	for (NVDpyId dpy = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
			!nvDpyIdIsInvalid(dpy);
			dpy = nvNextDpyIdInDpyIdListUnsorted(dpy, validDpys)) {
		if (!nvDpyIdsAreEqual(dpy, fContext.classicDpy))
			add(dpy);
	}

	fEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fEventFd < 0) {
		debug_printf("[!] NvMultiHead: eventfd failed (%s); no hot-plug events\n",
			strerror(errno));
	}
	debug_printf("nvidia_gsp: %" B_PRIu32 " display outputs\n", Count());
}


NvMultiHead::~NvMultiHead()
{
	// Turn off what this table turned on; output 0 is the classic code's.
	for (uint32 i = 1; i < Count(); i++) {
		Output &out = *fOutputs[i];
		uint32 hwHead = out.hwHead.load();
		if (hwHead == kNoHead)
			continue;
		try {
			SubmitMode(out, hwHead, nullptr, nullptr);
		} catch (const std::system_error &ex) {
			debug_printf("[!] NvMultiHead: turning off output %" B_PRIu32 ": %s\n",
				i, ex.what());
		}
	}
	if (fEventFd >= 0)
		close(fEventFd);
}


NvMultiHead::Output *
NvMultiHead::Extra(uint32 output)
{
	if (output == 0 || output >= Count())
		RaiseErrno(EINVAL);
	return fOutputs[output].get();
}


void
NvMultiHead::QueryDynamic(NVDpyId dpy, NvKmsQueryDpyDynamicDataReply &reply)
{
	// The params are large (the EDID buffer), so not on the stack.
	auto params = std::make_unique<NvKmsQueryDpyDynamicDataParams>();
	params->request.deviceHandle = fContext.kmsDev->Get();
	params->request.dispHandle = fContext.disp;
	params->request.dpyId = dpy;
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA,
		params.get(), sizeof(*params)));
	reply = params->reply;
}


static uint32
ToConnector(NvKmsConnectorType type)
{
	switch (type) {
		case NVKMS_CONNECTOR_TYPE_DP:
		case NVKMS_CONNECTOR_TYPE_USBC:	// DisplayPort alternate mode
		case NVKMS_CONNECTOR_TYPE_DP_SERIALIZER:
			return B_CONNECTOR_DISPLAYPORT;
		case NVKMS_CONNECTOR_TYPE_VGA:
		case NVKMS_CONNECTOR_TYPE_ADC:
			return B_CONNECTOR_VGA;
		case NVKMS_CONNECTOR_TYPE_DVI_I:
		case NVKMS_CONNECTOR_TYPE_DVI_D:
			return B_CONNECTOR_DVI;
		case NVKMS_CONNECTOR_TYPE_HDMI:
			return B_CONNECTOR_HDMI;
		case NVKMS_CONNECTOR_TYPE_LVDS:
		case NVKMS_CONNECTOR_TYPE_DSI:
			return B_CONNECTOR_EMBEDDED;
		default:
			return B_CONNECTOR_UNKNOWN;
	}
}


status_t
NvMultiHead::GetInfo(uint32 output, multi_head_info *info)
{
	if (output >= Count() || info == nullptr)
		return B_BAD_VALUE;
	Output &out = *fOutputs[output];
	*info = {};

	NvKmsQueryDpyStaticDataParams dpyStatic {};
	dpyStatic.request.deviceHandle = fContext.kmsDev->Get();
	dpyStatic.request.dispHandle = fContext.disp;
	dpyStatic.request.dpyId = out.dpy;
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &dpyStatic,
		sizeof(dpyStatic)));
	if (dpyStatic.reply.mobileInternal)
		info->flags |= B_HEAD_INTERNAL;

	NvKmsQueryConnectorStaticDataParams connector {};
	connector.request.deviceHandle = fContext.kmsDev->Get();
	connector.request.dispHandle = fContext.disp;
	connector.request.connectorHandle = dpyStatic.reply.connectorHandle;
	if (fContext.kms->Control(NVKMS_IOCTL_QUERY_CONNECTOR_STATIC_DATA, &connector,
			sizeof(connector)) >= 0) {
		info->connector = ToConnector(connector.reply.type);
		info->connector_index = connector.reply.typeIndex + 1;
	}

	NvKmsQueryDpyDynamicDataReply dynamic;
	QueryDynamic(out.dpy, dynamic);
	if (dynamic.connected)
		info->flags |= B_HEAD_CONNECTED;
	if (out.hwHead.load() != kNoHead)
		info->flags |= B_HEAD_ENABLED;
	if (dynamic.vrrType != NVKMS_DPY_VRR_TYPE_NONE)
		info->flags |= B_HEAD_VRR;
	if (dynamic.supportsHDR)
		info->flags |= B_HEAD_HDR;
	// RGB 4:4:4 is what the desktop scans out. gamma_size stays 0: Quasar
	// offers no gamma interface yet (R-11).
	info->max_bpc = dynamic.supportedOutputColorFormats.rgb444.maxBpc;
	info->width_mm = dynamic.physicalDimensions.widthInCM * 10;
	info->height_mm = dynamic.physicalDimensions.heightInCM * 10;

	if (dynamic.edid.valid && dynamic.edid.bufferSize >= 128) {
		edid1_info edid {};
		edid_decode(&edid, (const edid1_raw*)dynamic.edid.buffer);
		strlcpy(info->vendor, edid.vendor.manufacturer, sizeof(info->vendor));
		info->serial = edid.vendor.serial;
		for (const auto &monitor : edid.detailed_monitor) {
			if (monitor.monitor_desc_type == EDID1_MONITOR_NAME) {
				strlcpy(info->name, monitor.data.monitor_name, sizeof(info->name));
				break;
			}
		}
	}

	// The preferred mode and the refresh range, from NVKMS's mode list for
	// the dpy (the same list the mode hooks offer).
	if (dynamic.connected) {
		uint32 maxRefresh = 0;
		for (NvU32 i = 0;; i++) {
			NvKmsValidateModeIndexParams params {};
			params.request.deviceHandle = fContext.kmsDev->Get();
			params.request.dispHandle = fContext.disp;
			params.request.dpyId = out.dpy;
			params.request.modeIndex = i;
			if (fContext.kms->Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params,
					sizeof(params)) < 0 || params.reply.end)
				break;
			if (!params.reply.valid)
				continue;
			if (params.reply.preferredMode)
				info->preferred = ToHaikuMode(params.reply.mode);
			if (params.reply.mode.timings.RRx1k > maxRefresh)
				maxRefresh = params.reply.mode.timings.RRx1k;
		}
		// RRx1k is millihertz, as the header's fields.
		info->max_refresh_millihz = maxRefresh;
		if (info->flags & B_HEAD_VRR) {
			NvKmsGetDpyAttributeParams params {};
			params.request.deviceHandle = fContext.kmsDev->Get();
			params.request.dispHandle = fContext.disp;
			params.request.dpyId = out.dpy;
			params.request.attribute = NV_KMS_DPY_ATTRIBUTE_VRR_MIN_REFRESH_RATE;
			if (fContext.kms->Control(NVKMS_IOCTL_GET_DPY_ATTRIBUTE, &params,
					sizeof(params)) >= 0 && params.reply.value > 0)
				info->min_refresh_millihz = (uint32)params.reply.value * 1000;
		}
	}
	return B_OK;
}


status_t
NvMultiHead::GetEdid(uint32 output, void *data, size_t *_size)
{
	if (output >= Count() || _size == nullptr)
		return B_BAD_VALUE;
	NvKmsQueryDpyDynamicDataReply dynamic;
	QueryDynamic(fOutputs[output]->dpy, dynamic);
	if (!dynamic.edid.valid)
		return B_ENTRY_NOT_FOUND;
	size_t size = dynamic.edid.bufferSize;
	if (data == nullptr || *_size < size) {
		*_size = size;
		return B_BUFFER_OVERFLOW;
	}
	memcpy(data, dynamic.edid.buffer, size);
	*_size = size;
	return B_OK;
}


bool
NvMultiHead::ValidateMode(const Output &out, const NvKmsMode &mode)
{
	NvKmsValidateModeParams params {};
	params.request.deviceHandle = fContext.kmsDev->Get();
	params.request.dispHandle = fContext.disp;
	params.request.dpyId = out.dpy;
	params.request.modeValidation.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
	params.request.mode = mode;
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
	return params.reply.valid;
}


// The same list the classic hooks build for output 0, built when first
// asked for and again after a hot-plug.
void
NvMultiHead::LoadModes(Output &out)
{
	if (out.modesValid)
		return;
	out.modes.clear();
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fContext.kmsDev->Get();
		params.request.dispHandle = fContext.disp;
		params.request.dpyId = out.dpy;
		params.request.modeIndex = i;
		CheckErrno(fContext.kms->Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params,
			sizeof(params)));
		if (params.reply.end)
			break;
		if (!ValidateMode(out, params.reply.mode))
			continue;
		out.modes.push_back(params.reply.mode);
	}
	out.modesValid = true;
}


uint32
NvMultiHead::ModeCount(uint32 output)
{
	Output &out = *Extra(output);
	LoadModes(out);
	return (uint32)out.modes.size();
}


status_t
NvMultiHead::GetModeList(uint32 output, display_mode *modes)
{
	Output &out = *Extra(output);
	LoadModes(out);
	for (size_t i = 0; i < out.modes.size(); i++)
		modes[i] = ToHaikuMode(out.modes[i]);
	return B_OK;
}


uint32
NvMultiHead::PickHwHead(const Output &out) const
{
	uint32 used = 0;
	for (const auto &other : fOutputs) {
		uint32 head = other->hwHead.load();
		if (head != kNoHead)
			used |= 1U << head;
	}
	// The classic head belongs to output 0 even while it is off.
	if (&out != fOutputs[0].get())
		used |= 1U << fContext.classicHwHead;
	uint32 numHeads = fContext.kmsDev->Info().numHeads;
	for (uint32 head = 0; head < numHeads; head++) {
		if ((out.headMask & (1U << head)) != 0 && (used & (1U << head)) == 0)
			return head;
	}
	return kNoHead;
}


// The classic hooks set a mode on output 0 (always on the classic head).
void
NvMultiHead::ClassicModeSet()
{
	fOutputs[0]->hwHead = fContext.classicHwHead;
	fOutputs[0]->mirrorOf = B_NO_SOURCE_OUTPUT;
}


uint32
NvMultiHead::HardwareHeadCount() const
{
	return fContext.kmsDev->Info().numHeads;
}


// The frame buffer an output scans out: its own, output 0's from the
// classic code, or for a mirror its source's.
const NvKmsBitmap *
NvMultiHead::FrameBufferOf(uint32 output) const
{
	if (output >= fOutputs.size())
		return nullptr;
	const Output &out = *fOutputs[output];
	if (out.mirrorOf != B_NO_SOURCE_OUTPUT)
		return FrameBufferOf(out.mirrorOf);
	if (output == 0)
		return fContext.classicFrameBuffer();
	return out.frameBuffer.IsSet() ? &out.frameBuffer : nullptr;
}


// One head in one NVKMS request; the other heads keep their state. mode
// nullptr turns the head off (an empty dpy list).
void
NvMultiHead::SubmitMode(Output &out, uint32 hwHead, const NvKmsMode *mode,
	const NvKmsBitmap *frameBuffer)
{
	auto params = std::make_unique<NvKmsSetModeParams>();
	params->request.deviceHandle = fContext.kmsDev->Get();
	params->request.commit = true;
	params->request.requestedDispsBitMask = 1U << 0;
	params->request.disp[0].requestedHeadsBitMask = 1U << hwHead;
	auto &head = params->request.disp[0].head[hwHead];
	if (mode != nullptr) {
		head.dpyIdList = nvAddDpyIdToEmptyDpyIdList(out.dpy);
		head.mode = *mode;
		head.modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		head.viewPortOut = {.x = 0, .y = 0, .width = (NvU16)mode->timings.hVisible,
			.height = (NvU16)mode->timings.vVisible};
		head.viewPortSizeIn = {.width = (NvU16)mode->timings.hVisible,
			.height = (NvU16)mode->timings.vVisible};
		auto &layer = head.flip.layer[NVKMS_MAIN_LAYER];
		layer.surface.handle[0] = frameBuffer->Surface().Get();
		layer.surface.specified = true;
		layer.sizeIn.val = {.width = (NvU16)frameBuffer->Width(),
			.height = (NvU16)frameBuffer->Height()};
		layer.sizeIn.specified = true;
		layer.sizeOut.val = layer.sizeIn.val;
		layer.sizeOut.specified = true;
		// A modeset clears the cursor; carry this output's over.
		if (out.cursorVisible && fCursorSurface != 0) {
			head.flip.cursor.image.surfaceHandle[NVKMS_LEFT] = fCursorSurface;
			head.flip.cursor.image.cursorCompParams = CursorCompParams();
			head.flip.cursor.imageSpecified = true;
			head.flip.cursor.position = {
				.x = (NvS16)(out.cursorX - fCursorHotX),
				.y = (NvS16)(out.cursorY - fCursorHotY)};
			head.flip.cursor.positionSpecified = true;
		}
	} else {
		head.dpyIdList = nvEmptyDpyIdList();
	}

	if (fContext.kms->Control(NVKMS_IOCTL_SET_MODE, params.get(), sizeof(*params)) < 0) {
		int error = errno;
		debug_printf("[!] NvMultiHead: SET_MODE on head %" B_PRIu32 " failed: status %d,"
			" disp %d, head %d\n", hwHead, params->reply.status,
			params->reply.disp[0].status, params->reply.disp[0].head[hwHead].status);
		RaiseErrno(error);
	}
}


status_t
NvMultiHead::SetMode(uint32 output, const display_mode *mode)
{
	Output &out = *Extra(output);
	uint32 hwHead = out.hwHead.load();

	if (mode == nullptr) {
		if (hwHead == kNoHead)
			return B_OK;
		SubmitMode(out, hwHead, nullptr, nullptr);
		out.hwHead = kNoHead;
		out.mirrorOf = B_NO_SOURCE_OUTPUT;
		out.mode = {};
		out.oldFrameBuffer = NvKmsBitmap();
		out.frameBuffer = NvKmsBitmap();
		return B_OK;
	}

	// As the classic hook: no virtual desktop or panning.
	if (mode->virtual_width != mode->timing.h_display
		|| mode->virtual_height != mode->timing.v_display
		|| mode->h_display_start != 0 || mode->v_display_start != 0)
		return B_BAD_VALUE;
	NvKmsMode nvMode = ToNvKmsMode(*mode);
	if (!ValidateMode(out, nvMode))
		return B_BAD_VALUE;

	bool newHead = hwHead == kNoHead;
	if (newHead) {
		hwHead = PickHwHead(out);
		if (hwHead == kNoHead)
			return B_BUSY;	// every head this output can use is taken
	}

	NvKmsBitmap frameBuffer(*fContext.rmDev, *fContext.kmsDev, mode->virtual_width,
		mode->virtual_height, (color_space)mode->space);
	SubmitMode(out, hwHead, &nvMode, &frameBuffer);

	out.hwHead = hwHead;
	out.mirrorOf = B_NO_SOURCE_OUTPUT;
	out.mode = nvMode;
	out.dpmsOff = false;
	// The old frame buffer may still be read until the new mode's first
	// frame; keep it until the next change, as the classic code does.
	out.oldFrameBuffer = std::move(out.frameBuffer);
	out.frameBuffer = std::move(frameBuffer);
	return B_OK;
}


/*!	Version 2: every output at once (R-19). One NVKMS SET_MODE over all
	hardware heads, so NVKMS checks the configuration as a whole and applies
	all of it or none. Outputs not listed are turned off.

	Plan: each enabled output keeps its hardware head if it has one and the
	dpy can still use it, output 0 gets the classic head, the others the
	lowest free head in their headMask. More enabled outputs than heads, or
	an output that finds no head, gives B_BUSY. A mirror's mode is its
	source's, and it scans out the source's surface.

	TEST_ONLY sends the same request with commit false and without
	surfaces (NVKMS checks modes, heads and bandwidth; nothing is allocated).
*/
status_t
NvMultiHead::SetConfiguration(const multi_head_config *configs, uint32 count,
	uint32 flags)
{
	const uint32 outputs = Count();
	const uint32 numHeads = HardwareHeadCount();
	const bool testOnly = (flags & B_MULTI_HEAD_TEST_ONLY) != 0;
	if (configs == nullptr && count != 0)
		return B_BAD_VALUE;

	struct Plan {
		bool enabled = false;
		uint32 source = B_NO_SOURCE_OUTPUT;
		display_mode mode {};
		NvKmsMode nvMode {};
		uint32 hwHead = kNoHead;
	};
	std::vector<Plan> plan(outputs);

	uint32 enabled = 0;
	for (uint32 i = 0; i < count; i++) {
		const multi_head_config &config = configs[i];
		if (config.output >= outputs || plan[config.output].enabled)
			return B_BAD_VALUE;		// unknown, or listed twice enabled
		if ((config.flags & B_OUTPUT_CONFIG_ENABLED) == 0)
			continue;
		Plan &p = plan[config.output];
		p.enabled = true;
		p.source = config.source_output == config.output
			? B_NO_SOURCE_OUTPUT : config.source_output;
		p.mode = config.mode;
		enabled++;
	}
	if (enabled > numHeads)
		return B_BUSY;

	// Mirrors: the source must be enabled and not a mirror itself. Output 0
	// cannot be a mirror: the classic hooks would have no frame buffer of
	// their own to report (app_server can make output 0 the source instead).
	if (plan[0].enabled && plan[0].source != B_NO_SOURCE_OUTPUT)
		return B_NOT_SUPPORTED;
	for (uint32 i = 0; i < outputs; i++) {
		Plan &p = plan[i];
		if (!p.enabled || p.source == B_NO_SOURCE_OUTPUT)
			continue;
		if (p.source >= outputs || !plan[p.source].enabled
			|| plan[p.source].source != B_NO_SOURCE_OUTPUT)
			return B_BAD_VALUE;
		p.mode = plan[p.source].mode;
	}

	for (uint32 i = 0; i < outputs; i++) {
		Plan &p = plan[i];
		if (!p.enabled)
			continue;
		const display_mode &mode = p.mode;
		if (mode.virtual_width != mode.timing.h_display
			|| mode.virtual_height != mode.timing.v_display
			|| mode.h_display_start != 0 || mode.v_display_start != 0)
			return B_BAD_VALUE;
		p.nvMode = ToNvKmsMode(mode);
		if (!ValidateMode(*fOutputs[i], p.nvMode))
			return B_BAD_VALUE;
	}

	// Hardware heads: keep, then classic, then the lowest free.
	uint32 used = 0;
	for (uint32 i = 0; i < outputs; i++) {
		uint32 head = fOutputs[i]->hwHead.load();
		if (plan[i].enabled && head != kNoHead
			&& (i == 0 || head != fContext.classicHwHead)) {
			plan[i].hwHead = head;
			used |= 1U << head;
		}
	}
	if (plan[0].enabled && plan[0].hwHead == kNoHead
		&& (used & (1U << fContext.classicHwHead)) == 0) {
		plan[0].hwHead = fContext.classicHwHead;
		used |= 1U << fContext.classicHwHead;
	}
	for (uint32 i = 0; i < outputs; i++) {
		Plan &p = plan[i];
		if (!p.enabled || p.hwHead != kNoHead)
			continue;
		uint32 reserved = i == 0 ? 0 : 1U << fContext.classicHwHead;
		for (uint32 head = 0; head < numHeads; head++) {
			uint32 bit = 1U << head;
			if ((fOutputs[i]->headMask & bit) != 0 && ((used | reserved) & bit) == 0) {
				p.hwHead = head;
				used |= bit;
				break;
			}
		}
		if (p.hwHead == kNoHead)
			return B_BUSY;
	}

	// Frame buffers for the outputs that have their own (not for a test).
	std::vector<NvKmsBitmap> frameBuffers(outputs);
	if (!testOnly) {
		for (uint32 i = 0; i < outputs; i++) {
			const Plan &p = plan[i];
			if (p.enabled && p.source == B_NO_SOURCE_OUTPUT) {
				frameBuffers[i] = NvKmsBitmap(*fContext.rmDev, *fContext.kmsDev,
					p.mode.virtual_width, p.mode.virtual_height,
					(color_space)p.mode.space);
			}
		}
	}

	auto params = std::make_unique<NvKmsSetModeParams>();
	params->request.deviceHandle = fContext.kmsDev->Get();
	params->request.commit = !testOnly;
	params->request.requestedDispsBitMask = 1U << 0;
	params->request.disp[0].requestedHeadsBitMask = (1U << numHeads) - 1;
	for (uint32 head = 0; head < numHeads; head++)
		params->request.disp[0].head[head].dpyIdList = nvEmptyDpyIdList();
	for (uint32 i = 0; i < outputs; i++) {
		const Plan &p = plan[i];
		if (!p.enabled)
			continue;
		auto &head = params->request.disp[0].head[p.hwHead];
		const NvKmsMode &mode = p.nvMode;
		head.dpyIdList = nvAddDpyIdToEmptyDpyIdList(fOutputs[i]->dpy);
		head.mode = mode;
		head.modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		head.viewPortOut = {.x = 0, .y = 0, .width = (NvU16)mode.timings.hVisible,
			.height = (NvU16)mode.timings.vVisible};
		head.viewPortSizeIn = {.width = (NvU16)mode.timings.hVisible,
			.height = (NvU16)mode.timings.vVisible};
		if (testOnly)
			continue;
		const NvKmsBitmap &surface = frameBuffers[
			p.source == B_NO_SOURCE_OUTPUT ? i : p.source];
		auto &layer = head.flip.layer[NVKMS_MAIN_LAYER];
		layer.surface.handle[0] = surface.Surface().Get();
		layer.surface.specified = true;
		layer.sizeIn.val = {.width = (NvU16)surface.Width(),
			.height = (NvU16)surface.Height()};
		layer.sizeIn.specified = true;
		layer.sizeOut.val = layer.sizeIn.val;
		layer.sizeOut.specified = true;
		// A modeset clears the cursor; outputs 1.. carry theirs over here,
		// output 0's is put back by the classic code (adoptClassic).
		const Output &out = *fOutputs[i];
		if (i != 0 && out.cursorVisible && fCursorSurface != 0) {
			head.flip.cursor.image.surfaceHandle[NVKMS_LEFT] = fCursorSurface;
			head.flip.cursor.image.cursorCompParams = CursorCompParams();
			head.flip.cursor.imageSpecified = true;
			head.flip.cursor.position = {
				.x = (NvS16)(out.cursorX - fCursorHotX),
				.y = (NvS16)(out.cursorY - fCursorHotY)};
			head.flip.cursor.positionSpecified = true;
		}
	}

	if (fContext.kms->Control(NVKMS_IOCTL_SET_MODE, params.get(), sizeof(*params)) < 0) {
		debug_printf("[!] NvMultiHead: set_configuration%s rejected: status %d,"
			" disp %d\n", testOnly ? " (test)" : "", params->reply.status,
			params->reply.disp[0].status);
		for (uint32 head = 0; head < numHeads; head++) {
			if (params->reply.disp[0].head[head].status != NVKMS_SET_MODE_ONE_HEAD_STATUS_SUCCESS)
				debug_printf("[!]   head %" B_PRIu32 ": status %d\n", head,
					params->reply.disp[0].head[head].status);
		}
		return B_BAD_VALUE;
	}
	if (testOnly)
		return B_OK;

	// Committed: record the new state.
	for (uint32 i = 1; i < outputs; i++) {
		Output &out = *fOutputs[i];
		const Plan &p = plan[i];
		out.hwHead = p.enabled ? p.hwHead : kNoHead;
		out.mirrorOf = p.enabled ? p.source : B_NO_SOURCE_OUTPUT;
		out.mode = p.enabled ? p.nvMode : NvKmsMode {};
		if (p.enabled)
			out.dpmsOff = false;
		// The previous buffer may be read until the first frame of the new
		// configuration: keep it until the next change.
		out.oldFrameBuffer = std::move(out.frameBuffer);
		out.frameBuffer = std::move(frameBuffers[i]);
	}
	Output &zero = *fOutputs[0];
	zero.hwHead = plan[0].enabled ? plan[0].hwHead : kNoHead;
	zero.mirrorOf = plan[0].enabled ? plan[0].source : B_NO_SOURCE_OUTPUT;
	fContext.adoptClassic(plan[0].enabled ? &plan[0].nvMode : nullptr,
		std::move(frameBuffers[0]));
	return B_OK;
}


status_t
NvMultiHead::GetMode(uint32 output, display_mode *mode)
{
	Output &out = *Extra(output);
	if (out.hwHead.load() == kNoHead)
		return B_ENTRY_NOT_FOUND;
	*mode = ToHaikuMode(out.mode);
	if (const NvKmsBitmap *frameBuffer = FrameBufferOf(output))
		mode->space = frameBuffer->ColorSpace();
	return B_OK;
}


status_t
NvMultiHead::GetFrameBufferConfig(uint32 output, frame_buffer_config *config)
{
	Extra(output);
	const NvKmsBitmap *frameBuffer = FrameBufferOf(output);
	if (frameBuffer == nullptr)
		return B_ENTRY_NOT_FOUND;
	config->frame_buffer = frameBuffer->Bits();
	config->frame_buffer_dma = nullptr;
	config->bytes_per_row = frameBuffer->BytesPerRow();
	return B_OK;
}


status_t
NvMultiHead::SetDpms(uint32 output, uint32 dpmsFlags)
{
	Output &out = *Extra(output);
	NvS64 value;
	switch (dpmsFlags) {
		case B_DPMS_ON:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
			break;
		case B_DPMS_OFF:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_OFF;
			break;
		default:
			return B_BAD_VALUE;
	}
	NvKmsSetDpyAttributeParams params {};
	params.request.deviceHandle = fContext.kmsDev->Get();
	params.request.dispHandle = fContext.disp;
	params.request.dpyId = out.dpy;
	params.request.attribute = NV_KMS_DPY_ATTRIBUTE_DPMS;
	params.request.value = value;
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_SET_DPY_ATTRIBUTE, &params,
		sizeof(params)));
	out.dpmsOff = dpmsFlags == B_DPMS_OFF;
	return B_OK;
}


// Image and position on the output's head. An output that is off keeps
// its cursor state for when it is turned on.
void
NvMultiHead::ApplyCursor(Output &out)
{
	uint32 hwHead = out.hwHead.load();
	if (hwHead == kNoHead)
		return;

	NvKmsSetCursorImageParams image {};
	image.request.deviceHandle = fContext.kmsDev->Get();
	image.request.dispHandle = fContext.disp;
	image.request.head = hwHead;
	image.request.common.surfaceHandle[NVKMS_LEFT]
		= out.cursorVisible ? fCursorSurface : 0;
	image.request.common.cursorCompParams = CursorCompParams();
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_SET_CURSOR_IMAGE, &image,
		sizeof(image)));

	if (out.cursorVisible && fCursorSurface != 0) {
		NvKmsMoveCursorParams move {};
		move.request.deviceHandle = fContext.kmsDev->Get();
		move.request.dispHandle = fContext.disp;
		move.request.head = hwHead;
		move.request.common = {
			.x = (NvS16)(out.cursorX - fCursorHotX),
			.y = (NvS16)(out.cursorY - fCursorHotY)};
		CheckErrno(fContext.kms->Control(NVKMS_IOCTL_MOVE_CURSOR, &move,
			sizeof(move)));
	}
}


status_t
NvMultiHead::MoveCursor(uint32 output, int32 x, int32 y)
{
	Output &out = *Extra(output);
	out.cursorX = x;
	out.cursorY = y;
	uint32 hwHead = out.hwHead.load();
	if (hwHead == kNoHead || !out.cursorVisible || fCursorSurface == 0)
		return B_OK;

	NvKmsMoveCursorParams move {};
	move.request.deviceHandle = fContext.kmsDev->Get();
	move.request.dispHandle = fContext.disp;
	move.request.head = hwHead;
	move.request.common = {
		.x = (NvS16)(x - fCursorHotX),
		.y = (NvS16)(y - fCursorHotY)};
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_MOVE_CURSOR, &move, sizeof(move)));
	return B_OK;
}


status_t
NvMultiHead::ShowCursor(uint32 output, bool visible)
{
	Output &out = *Extra(output);
	if (out.cursorVisible == visible)
		return B_OK;
	out.cursorVisible = visible;
	ApplyCursor(out);
	return B_OK;
}


void
NvMultiHead::SetCursorImage(NvKmsSurfaceHandle surface, int32 hotX, int32 hotY)
{
	fCursorSurface = surface;
	fCursorHotX = hotX;
	fCursorHotY = hotY;
	for (uint32 i = 1; i < Count(); i++) {
		Output &out = *fOutputs[i];
		if (!out.cursorVisible)
			continue;
		try {
			ApplyCursor(out);
		} catch (const std::system_error &ex) {
			debug_printf("[!] NvMultiHead: cursor on output %" B_PRIu32 ": %s\n",
				i, ex.what());
		}
	}
}


NvHandle
NvMultiHead::ScanoutMemory(uint32 output, gpu_buffer_desc *desc)
{
	if (output == 0 || output >= Count())
		return 0;
	const NvKmsBitmap *frameBuffer = FrameBufferOf(output);
	if (frameBuffer == nullptr)
		return 0;
	*desc = {};
	desc->width = frameBuffer->Width();
	desc->height = frameBuffer->Height();
	desc->bytes_per_row = frameBuffer->BytesPerRow();
	desc->format = frameBuffer->ColorSpace();
	desc->modifier = 0;	// linear
	desc->offset = 0;
	return frameBuffer->Memory().Get();
}


uint32
NvMultiHead::HwHeadOf(uint32 output) const
{
	if (output >= fOutputs.size())
		return kNoHead;
	return fOutputs[output]->hwHead.load();
}


uint32
NvMultiHead::OutputOf(uint32 hwHead) const
{
	for (uint32 i = 0; i < fOutputs.size(); i++) {
		if (fOutputs[i]->hwHead.load() == hwHead)
			return i;
	}
	return kNoHead;
}


// NvPresent's event thread: NVKMS reported that a dpy was connected,
// removed or changed its EDID. Mode lists are rebuilt on the next request
// (app_server re-reads every output after the event); NVKMS itself keeps
// a head running on a disconnected dpy until it is told otherwise.
void
NvMultiHead::Hotplug()
{
	for (auto &out : fOutputs)
		out->modesValid = false;
	if (fEventFd >= 0)
		eventfd_write(fEventFd, 1);
}

#endif	// QUASAR_HAVE_MULTI_HEAD
