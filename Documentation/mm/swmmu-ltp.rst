=======================
SWMMU UML LTP testing
=======================

This document records the setup details needed to run the syscall LTP suite in
the SWMMU NOMMU UML guest. Keep the guest root filesystem, the matching UML
kernel modules, and host-side backing filesystems consistent for each run.

Host and guest shared-memory capacity
=====================================

LTP tests such as ``fallocate06`` can allocate large files through UML hostfs.
The host container's ``/dev/shm`` backs those operations; an earlier 1 GiB host
tmpfs attempt caused a ``Bus error`` in ``fallocate06``. Use an 8 GiB host
``/dev/shm`` as the current test baseline, and leave enough space in the host
``/tmp`` and overlay tmpfs as well. In the latest isolated run with the matching
loop module, ``fallocate06`` completed in 3.46 seconds without a bus error, but
Kirk reported the test as skipped (return code 0); this does not establish a
passing capacity result. Record the exact test outcome and reason for each run.
This host mount is separate from the guest's ``/dev/shm`` mount, which should be
mounted by the guest init with ``exec`` enabled for tests that execute files
there.

For example, the Docker options used for the host container were::

   --tmpfs /dev/shm:rw,exec,nosuid,nodev,size=8g
   --tmpfs /tmp:rw,exec,size=8g

The UML command line also needs enough guest memory for the suite (the recorded
run used ``mem=4g``). Monitor the host tmpfs usage during the run; a passing
small test does not establish that the full suite has sufficient capacity.

Coreutils and the ``id`` command
================================

The Alpine SWMMU package is ``coreutils-nommu`` built from the
``nommu-swmmu`` profile in ``thehajime/aports``. Use the APK from the matching
profile/release rather than Alpine's ordinary ``coreutils`` package. The
package contains ``/usr/bin/id`` and uses the compiler plugin/runtime expected
by that profile. Build it with ``SWMMU=1 SWMMU_ONLY=1 abuild -r`` from the
package directory, then install the resulting APK with the regular Alpine APK
tool while preparing the rootfs (as CI does). Add the matching package signing
key first, and allow replacement of BusyBox command links::

   cp /path/to/profile/keys/*.rsa.pub /etc/apk/keys/
   apk add --no-cache --no-scripts --force-overwrite \
           /path/to/packages/coreutils-nommu-<version>.apk

Install into a disposable image or rootfs overlay, not a shared base rootfs.
Run ``/usr/bin/id --version`` and ``/usr/bin/id`` in the UML guest before
starting LTP. The ordinary guest ``apk`` executable may not run correctly
under the target kernel; package installation belongs in the Alpine host
container during rootfs preparation. Save the APK version, SHA-256, output,
and exit status with the run log.

UML modules
===========

When a test kernel has ``CONFIG_MODULES=y`` and a needed driver such as
``CONFIG_BLK_DEV_LOOP=m``, install modules from the same configured kernel
build tree and include the resulting ``lib/modules/<kernel-release>`` in the
test container/rootfs. The CI workflow stages them with::

   make -j8 ARCH=um O=build modules_install \
           INSTALL_MOD_PATH="$PWD/.github/workflows"

It then packages ``.github/workflows/lib/modules`` with the UML test image.
For a local test, use a separate staging directory and bind-mount that
directory at ``/lib/modules`` in the host container that runs UML. Confirm
``modprobe loop`` succeeds in the guest and that ``/dev/loop0`` exists before
interpreting loop-dependent LTP cases. Do not reuse modules from another
kernel build just because ``uname -r`` matches; verify they came from the
build that produced the tested ``vmlinux``. If ``modules_install`` reports
that ``depmod`` is unavailable, install ``kmod`` in the host build container
and regenerate the module dependency indexes with ``depmod -b <stage>
<kernel-release>`` before mounting the staged tree.

The CI SAS configurations also enable ``CONFIG_UML_NET_VECTOR=y``. If network
tests are in scope, use a kernel built with that option and configure the
matching vector interface in the guest init.

Running and monitoring LTP
==========================

Run Kirk from the guest init after mounting proc, devpts, guest ``/dev/shm``
and ``/tmp``, loading required modules, and configuring any required network
interfaces. Preserve the full boot log and Kirk result JSON. For long full
syscall runs, avoid an outer host ``timeout`` when the goal is to measure
completion time; monitor the UML process and log instead. Record the start/end
time, final Kirk summary, UML exit status, kernel faults/panics, and any test
that stops producing output. If stopping a confirmed hung or panicked run,
record the reason and elapsed time rather than reporting a suite completion.

The host container may need ``--security-opt seccomp=unconfined`` for UML's
host operations. This changes the outer container's seccomp filter only; it is
independent of the guest's SWMMU mode (for example, ``zpoline=0`` or
``zpoline=1``).
