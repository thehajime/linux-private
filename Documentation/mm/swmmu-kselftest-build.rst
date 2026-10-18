=========================================
Codex Cloud UML NOMMU SWMMU test workflow
=========================================

This is the single setup and run guide for UML NOMMU SWMMU work in a Codex
Cloud environment. It covers the cloud host, Docker, kernel configuration,
Alpine userspace, KUnit, kselftest, LTP, and GDB. Record exact revisions,
configs, package hashes, commands, and full logs. A boot banner alone does not
prove that a guest test ran.

Workspace and preflight checks
==============================

The usual paths are::

   /workspace/linux-private       kernel checkout
   /workspace/aports              Alpine package checkout, when present
   /workspace/obsidian            experiment records
   /workspace/cloud-setup         disposable builds and root filesystems

Inspect branch, full HEAD, worktree state, remotes, free space, and existing
artifacts before changing anything. Preserve uncommitted work and local-only
commits. Keep source, kernel output, rootfs staging, and logs in separate paths.

Codex Cloud provides a managed Docker daemon. Test the intended local daemon
without changing Docker credentials or proxy state::

   env -u DOCKER_HOST -u DOCKER_CONTEXT -u DOCKER_TLS \
       -u DOCKER_TLS_VERIFY -u DOCKER_CERT_PATH \
       docker --host=unix:///var/run/docker.sock info

Kernel configuration requires ``flex`` and ``bison``. The compiler plugin also
requires matching GCC plugin headers, and guest selftests require a musl cross
compiler. Install these in the persistent environment setup and verify them::

   flex --version
   bison --version
   x86_64-linux-musl-gcc --version
   gcc --version

Some workspaces stage flex and bison below
``/workspace/cloud-setup/tool-root``. Select both executable and bison data
paths when using it::

   export PATH=/workspace/cloud-setup/tool-root/usr/bin:$PATH
   export BISON_PKGDATADIR=/workspace/cloud-setup/tool-root/usr/share/bison

Docker and UML host permissions
===============================

Hostfs and overlay runs need mount privileges, so use a disposable
``--privileged`` container. ``zpoline=1`` installs a low-address trampoline;
the UML host process needs ``CAP_SYS_RAWIO`` to map below the normal host
``mmap_min_addr`` restriction. ``--privileged`` includes it, or add it with
``--cap-add SYS_RAWIO``. GDB diagnostics also need ``--cap-add SYS_PTRACE``.

Use ``--security-opt seccomp=unconfined`` for the *outer Docker container*.
This removes Docker's default host syscall filter so UML can use its own
seccomp/SIGSYS mechanism. It does not select the guest SWMMU mode and does not
disable the UML kernel's own seccomp path. The diagnostic shape is::

   docker run --rm --privileged \
       --cap-add SYS_PTRACE --cap-add SYS_RAWIO \
       --security-opt seccomp=unconfined ...

Keep the managed Docker proxy and CA settings for image builds.

Kernel configuration and build
==============================

Start from ``arch/um/configs/x86_64_defconfig`` and use an external output
directory. The SAS profile requires at least::

   CONFIG_MMU=n
   CONFIG_NOMMU_SWMMU=y
   CONFIG_UML_NOMMU_SAS=y
   CONFIG_STATIC_LINK=y

Set or clear ``CONFIG_NOMMU_SWMMU_DEFAULT_ON`` explicitly. CI also enables
``CONFIG_UML_NET_VECTOR=y`` and builds with
``CFLAGS+="-DCONFIG_UML_NET_VECTOR"``. Use the same source revision and GCC
major version for the plugin and instrumented userspace.

CI appends ``tools/buildroot/ltp-add.config`` from the ``thehajime/ltp``
``fix-nommu`` branch. At the time of writing it requests::

   CONFIG_BLK_DEV_LOOP=y
   CONFIG_IKCONFIG=y
   CONFIG_IKCONFIG_PROC=y
   CONFIG_EXT3_FS=y
   CONFIG_EXT4_FS=y
   CONFIG_KEYS=y
   CONFIG_FANOTIFY=y
   CONFIG_FANOTIFY_ACCESS_PERMISSIONS=y
   CONFIG_XFS_FS=y
   CONFIG_BTRFS_FS=y
   CONFIG_VFAT_FS=y
   CONFIG_NLS_CODEPAGE_437=y
   CONFIG_NLS_ISO8859_1=y
   CONFIG_PID_NS=y
   CONFIG_USER_NS=y
   CONFIG_OVERLAY_FS=y

Inspect the current file before each CI comparison. Run ``olddefconfig`` and
save the resolved ``.config`` because dependencies may discard requests.
Loop setup adds boot and test time. A completion diagnostic may use the other
CI options with ``CONFIG_BLK_DEV_LOOP=n`` and leave loop-dependent cases
skipped or broken. Label that result. Restore ``CONFIG_BLK_DEV_LOOP=y`` for a
CI-matched run.

When modules are enabled, install them from the exact matching build::

   make -j8 ARCH=um O=build modules_install \
       INSTALL_MOD_PATH="$PWD/module-stage"

Do not reuse modules based only on ``uname -r``. If needed, install ``kmod`` in
the build container and run ``depmod -b module-stage <kernel-release>``.

Alpine NOMMU SWMMU root filesystem
==================================

Use ``.github/workflows/Dockerfile`` and its ``nommu-swmmu`` profile as the
reference. Packages come from matching ``thehajime/aports`` build artifacts:
``busybox-nommu``, ``musl-nommu``, ``libcrypto3``, ``libssl3``, and
``python3-nommu``. Copy the matching signing keys to ``/etc/apk/keys`` and
install the APKs while constructing the image. Record the aports ref, package
versions, and SHA-256 hashes.

Install packages in the Alpine build container or a disposable rootfs overlay.
Do not depend on the target guest's ``apk`` until that program has itself been
built and validated with the SWMMU profile. Use ``coreutils-nommu`` from the
same profile and allow it to replace BusyBox links::

   apk add --no-cache --no-scripts --force-overwrite \
       /path/to/coreutils-nommu-<version>.apk

Check ``/usr/bin/id --version`` and ``/usr/bin/id`` inside UML before Kirk;
Kirk calls ``id`` during scheduler setup. Keep a base rootfs read-only and put
each run's changes in a fresh overlay. Bind the init, tests, results, and
matching modules into that overlay.

Guest boot and init
===================

Hostfs boots use this argument shape::

   vmlinux root=/dev/root rootflags=/rootfs rootfstype=hostfs rw \
       mem=4g console=tty loglevel=8 zpoline=1 init=/tests/init

The outer container must mount the rootfs at ``rootflags``. The init must exist
and be executable. A minimal init mounts::

   mount -t proc proc /proc
   mkdir -p /dev/shm /dev/pts
   mount -t tmpfs -o rw,nosuid,nodev,exec tmpfs /dev/shm
   mount -t devpts devpts /dev/pts
   mount -t tmpfs tmpfs /tmp

Emit workload start, exit, elapsed-time, and completion markers. End with
``poweroff -f`` and save the UML host exit. With ``CONFIG_PANIC_TIMEOUT=0``, a
panic spins forever and can use a full host CPU. Stop it after preserving the
panic and active test; that is a panic rather than slow recovery.

Building SWMMU NOMMU kselftests
===============================

Build from ``tools/testing/selftests/nommu``. Export matching UAPI headers and
build the GCC plugin with the matching host compiler::

   mkdir -p build-kselftest
   make ARCH=um O=build headers_install \
       INSTALL_HDR_PATH=build-kselftest/uapi
   make -C tools/testing/selftests/nommu \
       SWMMU_BUILD_PLUGIN=1 SWMMU_HOSTCXX=g++ \
       SWMMU_PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" swmmu-plugin

Build a musl static PIE with all-access instrumentation::

   mkdir -p build-kselftest/nommu
   make -B -C tools/testing/selftests/nommu \
       OUTPUT="$PWD/build-kselftest/nommu" \
       CC=x86_64-linux-musl-gcc \
       KHDR_INCLUDES="-I$PWD/build-kselftest/uapi/include" \
       'USERCFLAGS=-include sys/prctl.h' USERLDFLAGS=-static-pie \
       SWMMU_PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
       SWMMU_ALL_ACCESS=1 SWMMU_RETURN_VALUE_LOAD=1 all

``SWMMU_RETURN_VALUE_LOAD`` is experimental. Its helper must issue a raw load
syscall so 64-bit data in the syscall errno range is preserved. The kernel
must implement ``NOMMU_SWMMU_ACCESS_RETURN_VALUE``.

For focused VMA expansion iteration, explicitly name the output target; its
short name can bypass output-specific plugin rules::

   make -B -C tools/testing/selftests/nommu \
       OUTPUT="$PWD/build-kselftest/nommu" \
       CC=x86_64-linux-musl-gcc \
       KHDR_INCLUDES="-I$PWD/build-kselftest/uapi/include" \
       'USERCFLAGS=-include sys/prctl.h' USERLDFLAGS=-static-pie \
       SWMMU_BUILD_PLUGIN=1 SWMMU_HOSTCXX=g++-14 \
       SWMMU_PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
       SWMMU_ALL_ACCESS=1 SWMMU_RETURN_VALUE_LOAD=1 \
       "$PWD/build-kselftest/nommu/nommu_swmmu_expand_test"

Verify lowering without a guest::

   make -C scripts/gcc-plugins/tests \
       PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
       OUTPUT="$PWD/build-kselftest/plugin-tests" all-access-return-value

KUnit and kselftest runs
========================

Disable ``CONFIG_KUNIT_UML_PCI`` for NOMMU KUnit as CI does. Keep KUnit and
guest kselftest boots separate because ``kunit_shutdown=halt`` can stop before
guest init. Save the resolved config, complete log, JSON or KTAP, plan,
pass/fail/skip totals, and host exit. ``KSFT_SKIP`` is not a pass. Confirm that
logs are fresh before copying them from an output directory.

LTP with Kirk
=============

Use the LTP tree from the matching ``thehajime/ltp`` revision. Mount it
read-only and preserve ``result.json`` and console output outside the rootfs.

The host container's ``/dev/shm`` backs large Hostfs activity. A 1 GiB host
tmpfs produced a bus error in ``fallocate06``. The current full-run baseline
is::

   --tmpfs /dev/shm:rw,exec,nosuid,nodev,size=8g
   --tmpfs /tmp:rw,exec,size=8g

This is separate from guest ``/dev/shm``. Use ``mem=4g`` for the recorded full
suite and monitor disk and tmpfs capacity.

For a loop run, use built-in loop support or exact matching modules, create
``/dev/loop0`` when needed, and validate ``modprobe loop`` before interpreting
filesystem tests. In a loop-free completion run, report affected tests.

Do not impose an outer timeout while measuring full-suite completion. Monitor
the process and log. Stop a confirmed panic or hang only after recording the
last test and elapsed time. A diagnostic may skip separately reproduced panic
tests, but preserve that skip file and do not call its totals an unmodified CI
run.

Kirk help and suite startup measure different paths. A result from
``python3 -S /opt/ltp/kirk --help`` omits Python ``site`` initialization and
only imports enough modules to print help. Time from ``kirk -f syscalls`` to
the first test includes normal Python startup, SUT probes, suite parsing, skip
filtering, scheduler setup, and kernel-log bookkeeping. Compare them only with
identical kernel, rootfs, Python command, pycache, Kirk tree, and boundaries.

GDB for dynamic musl/FDPIC faults
=================================

Run UML in the foreground under GDB and allow its expected SIGSYS relay::

   handle SIGSYS nostop noprint pass

Break immediately before ``start_thread`` in ``load_elf_fdpic_binary``. Save
``bprm->filename``, interpreter name, both ELF headers, entries, and FDPIC load
maps. Load the exact executable and interpreter with ``add-symbol-file`` at
addresses calculated from their load maps and section virtual addresses.
Verify Build IDs before using separate debug symbols. At the first fault save
``$_siginfo``, registers, fault address, instruction pointer, backtrace, and
surrounding instructions. For syscall diagnosis, record arguments and raw
kernel and libc return values.

For a fault after ``exec``, keep the exact guest executable and loader from the
rootfs and record their hashes and Build IDs before symbolizing. Capture the
guest ``/proc/<pid>/maps`` while the process is alive; host-side ``/proc`` shows
the UML process, not the UML guest mappings. For ``ET_DYN`` files, calculate
the load bias from a maps entry and the matching ``PT_LOAD`` segment's
``p_offset``/``p_vaddr`` (account for page alignment). Then add the runtime
address of the ELF ``.text`` section, not the load bias by itself. For
FDPIC, use the interpreter and executable load maps captured from
``load_elf_fdpic_binary``; their independently loaded segments do not share a
single conventional load bias.

Inspect the exact section and segment addresses before starting GDB::

   readelf -hW guest-program
   readelf -lW guest-program
   readelf -SW guest-program
   readelf -nW guest-program | rg 'Build ID'
   sha256sum guest-program guest-loader

For a conventional ``ET_DYN`` object, ``add-symbol-file`` takes the runtime
address of ``.text``. Pass runtime section addresses for other relocated
sections when needed::

   gdb -q ./vmlinux
   (gdb) set pagination off
   (gdb) add-symbol-file /exact/rootfs/path/libpython3.so.1.0 0xTEXT_RUNTIME \
         -s .data 0xDATA_RUNTIME -s .bss 0xBSS_RUNTIME
   (gdb) info files
   (gdb) info symbol 0xFAULT_IP
   (gdb) x/12i 0xFAULT_IP-16
   (gdb) bt

Use section addresses from the exact ELF and runtime mappings; do not copy
addresses from a different boot, rootfs, or ASLR layout. For the FDPIC
``start_thread`` breakpoint, save the load maps and calculate each image's
``.text`` runtime address from its own executable segment before issuing
``add-symbol-file`` for the interpreter and then the program. A stripped
binary can still be disassembled with its symbols loaded, but useful function
names require a separate debug file with a matching Build ID. Keep the GDB
command file and complete GDB/UML output with the run record.

Keep the normal FSGSBASE preload/setup used by a working UML launch. A fault
from a run that omitted it is a separate experiment.

Result records
==============

Record kernel branch and full SHA, config, tool versions, image hash, rootfs
and APK provenance, Docker options, UML arguments, init and skip files,
measurement boundaries, full output, structured results, guest and host exits,
and fault evidence. State whether a run is CI-matched, loop-free, focused, or
diagnostic. Commit useful commands, configs, init files, and raw logs so links
do not point only to a disposable workspace.
