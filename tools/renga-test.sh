#!/bin/bash
# One script for a test session on the Renga machine with the RTX 5070.
#
#   tools/renga-test.sh setup
#       Once, on a fresh Renga install: build tools, LLVM and the libraries
#       NVK and Zink build against (tools/renga-setup.sh).
#
#   tools/renga-test.sh build [--revision N]
#       Update the submodules, build everything, make the quasar package and
#       install it. Turns on the driver's verbose log for the test session.
#       Reboot afterwards.
#
#   tools/renga-test.sh run [--obs /path/to/obs/bin] [--skip-tests] [--video]
#       After the reboot: collect system state, run the GPU tests and the OBS
#       reproduction, and pack everything into ~/quasar-report-<date>.zip.
#       That one file is what to send back.
#       --video also decodes a short H.264 clip on the GPU (NVK's
#       experimental Vulkan Video, NVK_EXPERIMENTAL=video) with FFmpeg. Kept
#       separate because a GPU decoder fault can hang the machine; run it
#       last, after the rest of the session is in the report.
#
#   tools/renga-test.sh collect
#       Only collect logs and state (after a freeze and reboot, for example).
#
# Nothing here changes the system outside the package install, except the
# driver settings file ~/config/settings/kernel/drivers/nvidia_gsp, which the
# build step creates if it does not exist.
#
# Every step is bounded by a timeout, and a step that fails or hangs is
# recorded and skipped, so one bad step does not cost the session.

set -u

here="$(cd "$(dirname "$0")" && pwd)"
base="$(dirname "$here")"
cmd="${1:-}"
[ $# -gt 0 ] && shift

stamp="$(date +%Y%m%d-%H%M%S)"
report="${QUASAR_REPORT_DIR:-$HOME/quasar-report-$stamp}"
settingsFile="$HOME/config/settings/kernel/drivers/nvidia_gsp"

say() { echo "==> $*"; }

have() { command -v "$1" >/dev/null 2>&1; }

# kill_named <signal> <program name>: Haiku has no pkill by default. Its ps
# prints the team's command line first and "Id #Threads Gid Uid" last.
kill_named() {
	local sig=$1 name=$2
	if have pkill; then
		pkill "-$sig" -x "$name" 2>/dev/null
		return
	fi
	ps 2>/dev/null | awk -v n="$name" \
		'$1 ~ ("(^|/)" n "$") && NF >= 5 { print $(NF - 3) }' |
		while read -r id; do kill "-$sig" "$id" 2>/dev/null; done
}

# with_timeout <seconds> <output file> <command...>
# Runs the command with stdout and stderr in the file. Returns its exit
# status, or 124 after killing it on a timeout.
with_timeout() {
	local limit=$1 out=$2
	shift 2
	# Job control gives the background command its own process group, so a
	# timeout can kill the whole group: a test script and the binary it runs.
	set -m
	( "$@" >"$out" 2>&1 ) &
	local pid=$! waited=0
	set +m
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$waited" -ge "$limit" ]; then
			echo "[renga-test] timed out after ${limit}s: $*" >>"$out"
			kill -TERM -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null
			sleep 2
			kill -KILL -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null
			wait "$pid" 2>/dev/null
			return 124
		fi
		sleep 1
		waited=$((waited + 1))
	done
	wait "$pid"
}

# step <name> <seconds> <command...>: run, record the result in summary.txt.
step() {
	local name=$1 limit=$2
	shift 2
	say "$name"
	with_timeout "$limit" "$report/$name.txt" "$@"
	local status=$?
	case $status in
		0)   echo "PASS     $name" >>"$report/summary.txt" ;;
		124) echo "TIMEOUT  $name (${limit}s)" >>"$report/summary.txt" ;;
		*)   echo "FAIL($status) $name" >>"$report/summary.txt" ;;
	esac
	return $status
}

# capture <name> <command...>: record output, never counted as a test.
capture() {
	local name=$1
	shift
	with_timeout 60 "$report/$name.txt" "$@" || true
}

collect_state() {
	say "Collecting system state into $report"
	mkdir -p "$report"
	{
		echo "date: $(date)"
		echo "quasar tree: $(git -C "$base" rev-parse HEAD 2>/dev/null) ($(git -C "$base" rev-parse --abbrev-ref HEAD 2>/dev/null))"
		echo "mesa-nvk:    $(git -C "$base/mesa-nvk" rev-parse HEAD 2>/dev/null)"
		echo "mesa-zink:   $(git -C "$base/mesa-zink" rev-parse HEAD 2>/dev/null)"
		echo "nvidia tree: $(git -C "$base/open-gpu-kernel-modules" rev-parse HEAD 2>/dev/null)"
		echo "settings file ($settingsFile):"
		cat "$settingsFile" 2>/dev/null || echo "  (none)"
	} >"$report/versions.txt"

	capture uname uname -a
	capture sysinfo sysinfo
	capture listdev listdev
	capture packages sh -c 'pkgman search -i -a 2>/dev/null | grep -iE "quasar|nebula|mesa|vulkan|glvnd|llvm|libclc"'
	capture images-nouveau sh -c 'listimage 2>/dev/null | grep -iE "nouveau|nvidia|zink|VideoStreams|libvulkan|libEGL"'
	capture vulkan-layers sh -c 'ls -l /boot/system/add-ons/vulkan/implicit_layer.d /boot/home/config/add-ons/vulkan/implicit_layer.d /boot/system/non-packaged/add-ons/vulkan/implicit_layer.d /boot/home/config/non-packaged/add-ons/vulkan/implicit_layer.d 2>&1'
	capture vulkan-env sh -c 'env | grep -iE "^(VK_|MESA_|NVK_|ZINK_|GALLIUM_|LIBGL|EGL_)"; echo "--- icd.d"; ls -l /boot/system/add-ons/vulkan/icd.d /boot/home/config/add-ons/vulkan/icd.d /boot/system/non-packaged/add-ons/vulkan/icd.d /boot/home/config/non-packaged/add-ons/vulkan/icd.d 2>&1'

	# Driver lines from this boot and the previous one. grep -a: the syslog can
	# contain binary bytes.
	for f in /var/log/syslog /var/log/syslog.1 /var/log/previous_syslog; do
		[ -f "$f" ] || continue
		grep -a -iE "nvidia|NVRM|nvkms|Xid|accelerant|app_server.*(mode|screen)|KERN: (PANIC|vm_page_fault)" "$f" \
			| tail -n 4000 >"$report/$(basename "$f").driver.txt" 2>/dev/null
	done
	[ -f /var/log/syslog ] && tail -c 400000 /var/log/syslog >"$report/syslog.tail.txt" 2>/dev/null

	# Renga crash reports (kernel and application), newest twenty.
	if [ -d /boot/system/var/log/crash ]; then
		mkdir -p "$report/crash"
		ls -t /boot/system/var/log/crash 2>/dev/null | head -20 | while read -r f; do
			cp "/boot/system/var/log/crash/$f" "$report/crash/" 2>/dev/null
		done
	fi

	if have vulkaninfo; then
		with_timeout 60 "$report/vulkaninfo.txt" vulkaninfo || true
		with_timeout 60 "$report/vulkaninfo-summary.txt" vulkaninfo --summary || true
	else
		echo "vulkaninfo is not installed (pkgman install vulkan_tools)" >"$report/vulkaninfo.txt"
	fi
}

pack_report() {
	local zipFile="$report.zip"
	say "Packing $zipFile"
	if have zip; then
		( cd "$(dirname "$report")" && zip -q -r "$zipFile" "$(basename "$report")" )
	else
		zipFile="$report.tar.gz"
		tar -C "$(dirname "$report")" -czf "$zipFile" "$(basename "$report")"
	fi
	echo
	[ -f "$report/summary.txt" ] && cat "$report/summary.txt"
	echo
	echo "Send this file back: $zipFile"
}

do_build() {
	local revision=""
	while [ $# -gt 0 ]; do
		case "$1" in
			--revision) revision="$2"; shift 2 ;;
			*) echo "unknown option: $1" >&2; exit 2 ;;
		esac
	done
	mkdir -p "$report"
	cd "$base" || exit 1

	if ! "$here/renga-setup.sh" --check >"$report/setup-check.txt" 2>&1; then
		cat "$report/setup-check.txt"
		echo
		echo "Missing build prerequisites. Run: tools/renga-test.sh setup"
		exit 1
	fi

	say "Updating submodules"
	git submodule sync >"$report/build-submodules.txt" 2>&1
	if ! git submodule update --init >>"$report/build-submodules.txt" 2>&1; then
		echo "Submodule update failed; see $report/build-submodules.txt" >&2
		pack_report
		exit 1
	fi

	if [ ! -e "$settingsFile" ]; then
		say "Creating $settingsFile (verbose log for the test session)"
		mkdir -p "$(dirname "$settingsFile")"
		printf '# Quasar driver settings. See nvidia_gsp/nvidia/os-haiku-misc.cpp.\n# Remove "verbose true" for a quiet syslog.\nverbose true\n' >"$settingsFile"
	fi

	say "Building (this takes a while); log: $report/build.log"
	if ! ./Build.sh >"$report/build.log" 2>&1; then
		echo "BUILD FAILED" | tee -a "$report/summary.txt"
		grep -n -m3 -B2 -A40 -E "^FAILED|error:" "$report/build.log" >"$report/build-first-errors.txt" 2>/dev/null
		collect_state
		pack_report
		exit 1
	fi
	echo "PASS     build" >>"$report/summary.txt"

	say "Host tests (no GPU needed)"
	step host-sync-pool 300 "$base/tests/sync-pool/run.sh"
	step host-chan-report 300 "$base/tests/chan-report/run.sh"
	step host-renga-wsi-layer 300 "$base/tests/renga-wsi-layer/run.sh"
	step host-accelerant-cursor 300 "$base/tests/accelerant-cursor/run.sh"

	say "Making the package"
	local pkgArgs=()
	[ -n "$revision" ] && pkgArgs=(--revision "$revision")
	if ! tools/make-package.sh "${pkgArgs[@]}" >"$report/make-package.txt" 2>&1; then
		echo "FAIL     make-package" >>"$report/summary.txt"
		collect_state
		pack_report
		exit 1
	fi
	grep -i "mesa" "$report/make-package.txt"

	say "Installing the package"
	if ! tools/install-package.sh 2>&1 | tee "$report/install-package.txt"; then
		echo "FAIL     install-package" >>"$report/summary.txt"
		collect_state
		pack_report
		exit 1
	fi
	echo "PASS     package installed" >>"$report/summary.txt"
	collect_state
	pack_report
	echo
	echo "Now reboot, then run: tools/renga-test.sh run"
	echo "If Renga does not reach the desktop: hold Space at boot, choose"
	echo "\"Disable user add-ons\", then run tools/uninstall-package.sh --restore-previous"
	echo "(or --restore-nebula if no quasar was installed before)."
}

# Run one configuration of a test binary. Each run gets the stall reports
# (they are unconditional) and the debug notices.
probe_matrix() {
	local name=$1 limit=$2
	shift 2
	RENGA_WSI_DEBUG=1 NVK_NVRM_DEBUG=1 step "$name" "$limit" "$@"
	local status=$?
	[ $status -eq 0 ] && return 0
	# The default run failed or hung. The same test under settings that
	# each remove one suspect tells them apart in a single session:
	#   submit thread off  - the Vulkan runtime's threaded submit mode
	#   flushsync          - Zink's flush thread
	#   no threaded GL     - Gallium's threaded context
	NVK_NVRM_DEBUG=1 MESA_VK_ENABLE_SUBMIT_THREAD=0 step "$name.no-submit-thread" "$limit" "$@"
	NVK_NVRM_DEBUG=1 ZINK_DEBUG=flushsync step "$name.zink-flushsync" "$limit" "$@"
	NVK_NVRM_DEBUG=1 GALLIUM_THREAD=0 step "$name.no-gallium-thread" "$limit" "$@"
	return $status
}

# OBS's probe (renga branch of OBS-Studio_HaikuOS) follows libobs' real
# per-frame call order for the Screen Capture case and has switches that each
# remove one part of it. The default run reproduces the hang if it is there;
# when it fails, each switch is tried alone, and the ones that make it pass
# name the part of OBS's load that triggers the driver bug. --stall 5 makes a
# stuck step print "STALL:" with the GL call it is in, and exit 5.
obs_probe_matrix() {
	local dir=$1 probe="./obs-haiku-gl-probe"
	local common=(--verbose --stall 5 --timeout 60)
	( cd "$dir" && NVK_NVRM_DEBUG=1 step obs-gl-probe 120 "$probe" "${common[@]}" )
	local status=$?
	[ $status -eq 0 ] && return 0
	local opt
	for opt in --no-stage --no-stream --no-second-display --no-handoff \
			--fences "--fps 0"; do
		local name="obs-gl-probe${opt// /}"
		# shellcheck disable=SC2086
		( cd "$dir" && NVK_NVRM_DEBUG=1 step "$name" 120 "$probe" "${common[@]}" $opt )
	done
	# And the driver-side suspects, on the default load.
	( cd "$dir" && NVK_NVRM_DEBUG=1 MESA_VK_ENABLE_SUBMIT_THREAD=0 \
		step obs-gl-probe.no-submit-thread 120 "$probe" "${common[@]}" )
	( cd "$dir" && NVK_NVRM_DEBUG=1 ZINK_DEBUG=flushsync \
		step obs-gl-probe.zink-flushsync 120 "$probe" "${common[@]}" )
	( cd "$dir" && NVK_NVRM_DEBUG=1 GALLIUM_THREAD=0 \
		step obs-gl-probe.no-gallium-thread 120 "$probe" "${common[@]}" )
	return $status
}

do_run() {
	local obsDir="" skipTests=0 video=0
	while [ $# -gt 0 ]; do
		case "$1" in
			--obs) obsDir="$2"; shift 2 ;;
			--skip-tests) skipTests=1; shift ;;
			--video) video=1; shift ;;
			*) echo "unknown option: $1" >&2; exit 2 ;;
		esac
	done
	collect_state

	if [ $skipTests -eq 0 ]; then
		say "Building the GPU tests"
		if with_timeout 300 "$report/tests-build.txt" "$base/tests/build.sh"; then
			local t="$base/tests/build.$(getarch 2>/dev/null || uname -m)"
			probe_matrix vk-binary-wait-reuse 120 "$t/vk-binary-wait-reuse"
			probe_matrix vk-binary-wait-reuse.immediate 120 "$t/vk-binary-wait-reuse" --no-thread
			probe_matrix egl-threads 120 "$t/egl-surfaceless-threads" --seconds 45
			probe_matrix egl-threads.30fps 90 "$t/egl-surfaceless-threads" --seconds 20 --fps 30
			probe_matrix egl-threads.pbuffer 90 "$t/egl-surfaceless-threads" --seconds 20 --pbuffer
		else
			echo "FAIL     tests-build" >>"$report/summary.txt"
		fi
	fi

	# What NVK offers with its experimental video decode turned on: the
	# video queue family and VK_KHR_video_decode_h264 should appear. Only
	# device enumeration; no video work is submitted.
	if have vulkaninfo; then
		NVK_EXPERIMENTAL=video NVK_NVRM_DEBUG=1 \
			with_timeout 60 "$report/vulkaninfo-video.txt" vulkaninfo || true
	fi

	# OBS: its own driver probe first, then the real program.
	if [ -z "$obsDir" ]; then
		for d in "$HOME/OBS-Studio_HaikuOS/build/rundir/RelWithDebInfo/bin" \
				/boot/home/OBS-Studio_HaikuOS/build/rundir/RelWithDebInfo/bin \
				/boot/system/apps/obs-studio/bin /boot/home/config/apps/obs-studio/bin; do
			[ -x "$d/obs" ] && { obsDir="$d"; break; }
		done
	fi
	if [ -n "$obsDir" ] && [ -x "$obsDir/obs-haiku-gl-probe" ]; then
		obs_probe_matrix "$obsDir"
	else
		echo "SKIPPED  obs-gl-probe (no OBS build found; pass --obs <bin dir>)" >>"$report/summary.txt"
	fi

	if [ -n "$obsDir" ] && [ -x "$obsDir/obs" ]; then
		echo
		echo "------------------------------------------------------------------"
		echo "OBS will start on the GPU path with its watchdog off."
		echo "  1. Add a Screen Capture source (Sources, +, Screen Capture, OK)."
		echo "  2. Wait until it freezes, or one minute if it keeps working."
		echo "  3. Come back to this Terminal and press Enter."
		echo "------------------------------------------------------------------"
		( cd "$obsDir" && OBS_HAIKU_GL=egl OBS_HAIKU_GL_WATCHDOG=0 NVK_NVRM_DEBUG=1 RENGA_WSI_DEBUG=1 \
			./obs --verbose >"$report/obs-run.txt" 2>&1 ) &
		local obsShell=$!
		sleep 20
		# Which NVK and Zink this OBS loaded, while it is running.
		listimage 2>/dev/null | grep -iE "nouveau|zink|libvulkan|VideoStreams|libEGL|libgallium" \
			>"$report/obs-images.txt" 2>&1
		read -r -p "Press Enter when done (type h first if it froze): " answer
		# Give a stalled wait time to print its channel report (first at 3 s,
		# the full channel state again every 30 s).
		[ "$answer" = h ] && { echo "Waiting 35 s for the driver's stall report"; sleep 35; }
		echo "obs: user answered '$answer' (h = it froze)" >>"$report/summary.txt"
		kill_named TERM obs
		sleep 3
		kill_named KILL obs
		kill "$obsShell" 2>/dev/null
		grep -c "CPU wait not satisfied" "$report/obs-run.txt" 2>/dev/null \
			| sed 's/^/obs: stall reports in obs-run.txt: /' >>"$report/summary.txt"
		local obsLogs="$HOME/config/settings/obs-studio/logs"
		if [ -d "$obsLogs" ]; then
			mkdir -p "$report/obs-logs"
			ls -t "$obsLogs" | head -3 | while read -r f; do cp "$obsLogs/$f" "$report/obs-logs/"; done
		fi
	else
		echo "SKIPPED  obs (no OBS build found; pass --obs <bin dir>)" >>"$report/summary.txt"
	fi

	# State again after the tests: new syslog lines and crash reports.
	report="$report/after" collect_state
	pack_report

	# Last, and only on request: GPU video decode can hang the machine, and
	# the report above is already packed if it does.
	[ $video = 1 ] && run_video_decode
}

# H.264 decode through NVK's Vulkan Video with FFmpeg's Vulkan hwaccel,
# checked frame by frame against FFmpeg's software decoder: the frames,
# brought back as NV12, must have the same checksums.
run_video_decode() {
	say "Video decode (NVK_EXPERIMENTAL=video)"
	local dir="$report/video"
	mkdir -p "$dir"
	if ! have ffmpeg; then
		echo "SKIPPED  video-decode (ffmpeg not installed)" >>"$report/summary.txt"
		pack_report; return
	fi
	ffmpeg -hide_banner -hwaccels >"$dir/hwaccels.txt" 2>&1
	if ! grep -qx "vulkan" "$dir/hwaccels.txt"; then
		echo "SKIPPED  video-decode (this ffmpeg has no Vulkan hwaccel)" >>"$report/summary.txt"
		pack_report; return
	fi
	# A clip made here, so no file needs to be sent along: 3 s of FFmpeg's
	# test pattern at 1280x720, with whichever H.264 encoder is built in.
	local enc=""
	for e in libx264 libopenh264; do
		ffmpeg -hide_banner -h encoder=$e 2>/dev/null | grep -q "^Encoder $e" && { enc=$e; break; }
	done
	if [ -z "$enc" ] || ! with_timeout 120 "$dir/make-clip.txt" ffmpeg -y -hide_banner \
			-f lavfi -i testsrc2=size=1280x720:rate=30 -t 3 -c:v $enc \
			-pix_fmt yuv420p "$dir/clip.mp4"; then
		echo "SKIPPED  video-decode (could not make an H.264 clip; encoder: ${enc:-none})" >>"$report/summary.txt"
		pack_report; return
	fi
	with_timeout 120 "$dir/software.txt" ffmpeg -y -hide_banner -i "$dir/clip.mp4" \
		-vf format=nv12 -f framemd5 "$dir/software.framemd5"
	NVK_EXPERIMENTAL=video NVK_NVRM_DEBUG=1 step video-decode 120 \
		ffmpeg -y -hide_banner -init_hw_device vulkan=gpu -filter_hw_device gpu \
		-hwaccel vulkan -hwaccel_output_format vulkan -i "$dir/clip.mp4" \
		-vf hwdownload,format=nv12 -f framemd5 "$dir/vulkan.framemd5"
	cp "$report/video-decode.txt" "$dir/" 2>/dev/null
	if [ -s "$dir/vulkan.framemd5" ] && [ -s "$dir/software.framemd5" ]; then
		if diff <(grep -v '^#' "$dir/software.framemd5" | awk -F, '{print $NF}') \
				<(grep -v '^#' "$dir/vulkan.framemd5" | awk -F, '{print $NF}') \
				>"$dir/framemd5.diff"; then
			echo "PASS     video-decode-matches-software" >>"$report/summary.txt"
		else
			echo "FAIL     video-decode-matches-software ($(grep -c '^<' "$dir/framemd5.diff") frames differ)" >>"$report/summary.txt"
		fi
	fi
	report="$report/after-video" collect_state
	pack_report
}

case "$cmd" in
	setup)   exec "$here/renga-setup.sh" "$@" ;;
	build)   do_build "$@" ;;
	run)     do_run "$@" ;;
	collect) collect_state; pack_report ;;
	*)
		sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
		exit 2 ;;
esac
