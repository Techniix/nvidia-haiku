# Testing the renga branch on hardware

## Before the first session (once per fresh Renga install)

```sh
git clone -b renga https://github.com/Techniix/nvidia-haiku
cd nvidia-haiku
tools/renga-test.sh setup
```

`setup` installs the build tools, LLVM with clang and the Python modules
Mesa's generators need, checks out the submodules, and builds libglvnd,
libclc and SPIRV-LLVM-Translator into `deps.<arch>`. Building libclc can take
about an hour; it is done once. `tools/renga-setup.sh --check` reports what
is missing without changing anything.

## A session

```sh
tools/renga-test.sh build      # build, package, install; then reboot
tools/renga-test.sh run        # after the reboot
```

`run` ends with the path of `~/quasar-report-<date>.zip`. That file is the
result of the session. `run` stops once for OBS: add a Screen Capture source,
wait for a freeze or one minute, press Enter. `--obs <bin dir>` points it at
an OBS build it does not find; `--skip-tests` runs only the state collection
and OBS.

After a freeze and reboot, `tools/renga-test.sh collect` gathers the logs and
Renga's crash reports.

## Going back

The installer keeps a copy of the quasar package it replaces.

```sh
tools/uninstall-package.sh --restore-previous   # the quasar installed before
tools/uninstall-package.sh --restore-nebula     # if there was no quasar before
```

If Renga does not reach the desktop, hold Space at boot, choose "Disable user
add-ons" (quasar is in the home packages, so it is skipped), and run one of
the commands above.

If only presentation is broken (applications start but windows stay empty),
rebuild with the old window-system layer instead of going back entirely:

```sh
QUASAR_WSI=videostreams ./Build.sh
QUASAR_WSI=videostreams tools/make-package.sh
tools/install-package.sh
```

## What a first session answers

- whether this branch builds and boots on Renga;
- the OBS Screen Capture hang: which probe switches and driver settings make
  it pass, and the channel report from NVK;
- whether the Renga WSI presents (Vulkan and Zink applications);
- the three open points of `RENGA-REQUESTS.md` R-1 that need the card
  (layers per head, flips waiting in the kernel, VRR flip timing) are not
  measured automatically yet;
- `vulkaninfo`, for the Eglantine extension list.
