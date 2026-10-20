.. SPDX-License-Identifier: GPL-2.0

.. contents:: :local:

Execution model
===============

When emulating a system with an MMU, UML gives every ``mm_struct`` its own
host process with its own host address space, and keeps that address space
in sync with the kernel's page tables by issuing ``mmap()``/``munmap()``
into it.

NOMMU has no per-process address spaces: there is a single "physical" address
space that the kernel and every userspace process share.  UML models this with
a **userspace runner** - an ordinary uml-userspace stub host process, started
exactly like an MMU userspace process, but with the whole of "physical" memory
(the physmem file) mapped into it at the kernel's addresses:

- The runner ``exec()``'s the stub just like the MMU case.  The only extra step
  is that, once it is up, the kernel maps the entire physmem file into it in one
  go, at the same addresses the kernel uses.
- Because NOMMU never remaps, that single mapping is all a runner ever needs:
  there is no per-mm host process, no TLB sync and no ``mmap()`` of individual
  page-table entries into a child.  Userspace code and data - placed in memory
  by the loader - are directly executable and accessible.
- The runner processes hold no per-task state: the kernel loads a task's state
  on every entry, so there's just one process per CPU.

Running a task's userspace therefore only means pointing a runner at that
task's code and letting it run until the next syscall or signal

Building and running
====================

Configure and build the ARCH=um kernel normally, but deselect CONFIG_MMU.

KUnit tests can be run in NOMMU mode with::

   ./tools/testing/kunit/kunit.py run \
       --kconfig_add CONFIG_MMU=n --kconfig_add CONFIG_KUNIT_UML_PCI=n

Running a normal userspace requires NOMMU-aware binaries.  There is no stock
x86_64 NOMMU distribution, but a prebuilt Alpine image with musl-libc and
busybox built for NOMMU is available and can be turned into a root image::

   cid=$(docker create ghcr.io/thehajime/alpine:3.20.3-um-nommu)
   docker export "$cid" > alpine.tar
   docker rm "$cid"
   mkdir alpine-root && tar xf alpine.tar -C alpine-root
   mke2fs -q -F -t ext4 -d alpine-root alpine.ext4 1200M

Then boot it::

   ./linux ubd0=./alpine.ext4 root=/dev/ubda rw mem=1024m init=/sbin/init

Time-travel mode for test runs
==============================

NOMMU UML can enable ``CONFIG_UML_TIME_TRAVEL_SUPPORT`` on 64-bit builds.
The option is available when SMP is disabled.  For a test kernel, start with
the x86_64 configuration and enable the option, for example::

   make ARCH=um SUBARCH=x86_64 x86_64_defconfig
   scripts/config --disable MMU --disable SMP --enable UML_TIME_TRAVEL_SUPPORT
   make ARCH=um SUBARCH=x86_64 olddefconfig
   make ARCH=um SUBARCH=x86_64 -j$(nproc)

Boot with basic time-travel mode to skip idle periods while retaining host
timer interrupts while guest code is running::

   ./linux ubd0=./alpine.ext4 root=/dev/ubda rw mem=1024m \
       init=/sbin/init time-travel

For LTP, mount procfs and tmpfs before starting the suite.  On NOMMU, the
kernel's tiny-shmem implementation backs tmpfs with ramfs, which supports the
shared mappings used by LTP's test library.  For example, the LTP runner can
mount ``tmpfs`` on ``/dev/shm`` before invoking Kirk.

This is useful for reducing LTP wall-clock time when tests spend time sleeping
or waiting for timers.  CPU-bound tests still take host CPU time.  Do not use
``time-travel=inf-cpu`` for a general LTP run: a test that remains runnable and
uses ``CLOCK_MONOTONIC`` as its termination condition may not make sufficient
time progress.  Infinite-CPU mode is suitable only for workloads whose CPU
execution and simulated-time advancement are explicitly coordinated.
