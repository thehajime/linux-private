Occupied-target MREMAP_FIXED
----------------------------

MREMAP_FIXED with an occupied destination is currently unsupported for the
SWMMU path.

The current remap flow removes the destination mapping before preparing and
executing the source move. If source move preparation, pagetable allocation,
page copying, or destination insertion fails, restoring the original
destination mapping is not transactional.

This limitation is not inherently SWMMU-specific. The MMU implementation also
does not currently provide the shared transactional behavior required for this
case. Implementing occupied-target MREMAP_FIXED should therefore be handled in
a separate MMU/SWMMU patchset.

Until then, MREMAP_FIXED with an occupied destination remains unsupported and
its test is skipped. Empty-target MREMAP_FIXED remains supported.

Fault and error behavior
------------------------

SWMMU checked accesses support sizes of 1, 2, 4, and 8 bytes. The complete
access is validated before data is copied.

A successful load returns zero and writes the loaded value to the result
argument unless ``NOMMU_SWMMU_ACCESS_RETURN_VALUE`` is set. A successful
store returns zero.

The access syscall supports these flags:

``NOMMU_SWMMU_ACCESS_CHECKED``
   Return an error to the caller without delivering a signal.

``NOMMU_SWMMU_ACCESS_SIGNAL``
   Convert SWMMU address and permission faults into synchronous ``SIGSEGV``
   delivery.

``NOMMU_SWMMU_ACCESS_DYNAMIC``
   Allow the access to target either an ordinary NOMMU mapping or an SWMMU
   mapping. This mode validates VMA coverage and permissions for both.

``NOMMU_SWMMU_ACCESS_RETURN_VALUE``
   For loads, return the loaded 64-bit value directly from the syscall instead
   of copying it to the result pointer. This flag requires
   ``NOMMU_SWMMU_ACCESS_SIGNAL`` and a NULL result pointer. Access errors are
   delivered as ``SIGSEGV`` so a successful load can return any 64-bit value,
   including values in the syscall error range. Userspace must issue the
   syscall directly and must not pass its raw result through libc's
   ``syscall()`` errno conversion. The caller must pass a valid access size and
   flags because malformed arguments still return an errno value. This ABI is
   available only when ``long`` is 64 bits. Stores and compare-exchange reject
   this flag.

The checked-access error contract is:

``-EINVAL``
   The access size, flags, or another argument is invalid.

``-EFAULT``
   The address is unmapped, outside a SWMMU VMA, or exceeds the mapped range.

``-EACCES``
   The access violates the mapping protection.

``-EOPNOTSUPP``
   SWMMU mode is disabled for the current process.

For signal-mode accesses, the kernel maps SWMMU faults as follows:

``-EFAULT``
   ``SIGSEGV`` with ``si_code`` set to ``SEGV_MAPERR``.

``-EACCES``
   ``SIGSEGV`` with ``si_code`` set to ``SEGV_ACCERR``.

The faulting address is reported through ``siginfo_t.si_addr``. Invalid
arguments and disabled SWMMU mode remain ordinary syscall errors and do not
generate ``SIGSEGV``.

When a signal handler returns, the faulting access is retried. The default
``SIGSEGV`` disposition terminates the process.

Kernel mapping inconsistencies are internal errors and are not user-visible
SWMMU access faults.

UML configuration scope
-----------------------

The current SWMMU host-alias implementation targets the SAS UML
configuration:

.. code-block:: text

   CONFIG_MMU=n
   CONFIG_UML_NOMMU_SAS=y
   CONFIG_NOMMU_SWMMU=y

SWMMU itself is only available when ``CONFIG_MMU=n``. The combination of
``CONFIG_MMU=y`` and ``CONFIG_NOMMU_SWMMU=y`` is therefore not supported by
Kconfig and is not a target configuration.

The non-SAS NOMMU UML configuration is deferred:

.. code-block:: text

   CONFIG_MMU=n
   CONFIG_UML_NOMMU_SAS=n
   CONFIG_NOMMU_SWMMU=y

In non-SAS mode, userspace executes in a separate UML runner process. Host
aliases for SWMMU ranges must therefore be synchronized into that runner
through the existing ``current_mm_sync()`` and userspace-runner mapping path.
The current Option-A implementation maps aliases in the SAS host address
space only.

For SAS mode, host aliases are active mappings for the currently running
``mm_struct``. They are not permanent mappings for every SWMMU address space.
When the current task changes, the active host aliases must be replaced with
the aliases belonging to the new ``mm_struct``.

Per-VMA host-alias eligibility
------------------------------

An SWMMU VMA is eligible for a kernel direct alias copy only when its alias is
currently mapped for the active ``mm_struct``, the alias starts at the VMA's
virtual start, the range metadata covers the whole VMA, and the range
permissions allow the requested read or write. The kernel holds the SWMMU
space read lock while using that alias so an address-space switch cannot
replace it during the copy. If any condition fails, the user-copy path uses
the SWMMU pagetable translation path.

This is an internal kernel user-copy path. It does not remove
compiler-generated load/store helper calls from userspace. A userspace runtime
fast path would also need a way to discover per-VMA eligible ranges and keep
that information current across ``mmap()``, ``mprotect()``, ``munmap()``,
``mremap()``, and address-space switches; the current kernel exposes no such
ABI. KUnit coverage checks the eligibility predicate and its boundaries.

Explicit native-pointer compiler contract
-----------------------------------------

The SWMMU GCC plugin accepts the type attribute ``swmmu_native_ptr`` as an
explicit assertion that a pointer always refers to memory directly accessible
by ordinary userspace loads and stores. Under all-access instrumentation, a
dereference through this pointer type is emitted as a native instruction;
ordinary pointers and SWMMU pointers continue to use SWMMU helpers. Pointer
aliases and pointer arithmetic preserve the assertion, while a control-flow
merge with a pointer of a different state falls back to a helper. Loading a
native pointer value from a SWMMU structure still reads that field through a
helper and can use the native contract for the pointee; global pointer loads
currently remain conservatively dynamic.

For example, a userspace build can declare a trusted pointer type as:

.. code-block:: c

   typedef void * __attribute__((swmmu_native_ptr)) native_ptr;

The attribute is a caller-supplied promise, not a check of the address or its
VMA. Applying it to a pointer whose target can be an SWMMU mapping can bypass
SWMMU translation and fault or corrupt memory. A valid contract must cover the
target mapping's full lifetime, including allocator reuse, remapping, process
fork behavior, and calls across library boundaries. The kernel currently has
no userspace ABI to establish or maintain that native-VMA guarantee. A live
Kirk map snapshot showed that all VMAs in the tested default-off profile were
below the SWMMU range, but the default-on profile uses SWMMU mappings. This
snapshot is evidence for a profile-specific experiment, not a general CPython
or musl pointer contract.

Experimental allocator-result provenance
-----------------------------------------

The all-access plugin has opt-in allocator provenance controls for a build
whose runtime guarantees that selected allocator results always refer to
ordinary native mappings:

``-fplugin-arg-swmmu_plugin-native-allocator-results=1``
   Treat results from ``malloc``, ``calloc``, ``realloc`` (only when its input
   is already native), and declarations carrying ``swmmu_allocator_result`` as
   native.

``-fplugin-arg-swmmu_plugin-native-allocator-function=NAME``
   Treat a call returning a pointer from the named function as native. Pass the
   option once for each allocator function. This is intended for application
   allocators such as CPython's ``PyObject_Malloc`` when the selected runtime
   profile proves their returned arenas are native.

``-fplugin-arg-swmmu_plugin-native-reallocator-function=NAME``
   Treat a call to the named realloc function as native only when its input
   pointer already has native provenance. This keeps unknown or SWMMU inputs
   on the helper path.

The plugin propagates this state through pointer aliases. A merge with an
ordinary, SWMMU, or otherwise dynamic pointer keeps conservative helper
lowering. These options are assertions made by the build profile; the compiler
does not inspect mappings or check the allocator implementation. They are
unsafe when an allocator can return an SWMMU mapping, including a default-on
SWMMU heap, or when an allocator can be replaced with an implementation that
returns a different mapping class. Validate the complete allocator family,
including reallocations, custom hooks, mapping changes and fork behavior, for
each runtime profile before use.

The focused compiler check is:

.. code-block:: sh

   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" \
           all-access-native-allocator-results

This only checks generated code and conservative fallback paths. Guest tests
must verify the allocator's actual mapping class and process behavior.

Experimental native automatic-storage lowering
----------------------------------------------

The plugin option ``-fplugin-arg-swmmu_plugin-native-stack=1`` treats direct
accesses to automatic local variables, and pointers derived from their
addresses, as native while retaining all-access helper lowering for other
pointers. This reflects the current UML SAS execution stack and removes helper
calls for compiler-visible local scalar, array, and aggregate storage. GIMPLE
lifetime ``CLOBBER`` markers are metadata and are left unchanged rather than
lowered to an aggregate helper call.

This option is experimental and must be measured on the target application.
It does not prove that allocator results, Python objects, globals, TLS, escaped
unknown pointers, or a VMA selected only from its address are native. Those
accesses continue to use helpers. It also does not cover stack accesses added
after GIMPLE lowering, including target stack-clash probes. Guest runtime tests
must cover process creation and stack switching before enabling it as a general
userspace profile.

The focused compiler check is:

.. code-block:: sh

   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" \
           all-access-native-stack

Testing
-------

SWMMU UML testing uses the NOMMU selftests in
``tools/testing/selftests/nommu``. Build the kernel with the SAS and SWMMU
options enabled, then build the GCC plugin and install the tests into a
separate output directory. The host compiler must have GCC plugin headers
installed for its GCC version.

For example, configure the kernel output directory with:

.. code-block:: sh

   make ARCH=um O=build defconfig
   scripts/config --file build/.config --disable MMU \
           --enable NOMMU_SWMMU \
           --enable GCC_PLUGINS \
           --enable GCC_PLUGIN_SWMMU \
           --enable NOMMU_SWMMU_KUNIT_TEST \
           --enable NOMMU_SWMMU_DEFAULT_ON \
           --enable UML_NOMMU_SAS \
           --enable BINFMT_ELF_FDPIC
   make ARCH=um O=build -j32

Build the selftest plugin with:

.. code-block:: sh

   make -C tools/testing/selftests/nommu \
           SWMMU_BUILD_PLUGIN=1 SWMMU_HOSTCXX=g++ \
           SWMMU_PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           swmmu-plugin

To compile all-access scalar loads to the direct-return helper, also pass
``SWMMU_RETURN_VALUE_LOAD=1`` when building ``nommu_swmmu_test``. The plugin
option is ``-fplugin-arg-swmmu_plugin-return-value-load=1``. The userspace
runtime must provide ``nommu_swmmu_load_return_value`` and use a raw syscall
instruction so data values in the errno range are preserved. This option
changes scalar loads only; stores and bulk memory helpers retain their existing
interfaces.

The cross-build, rootfs staging, and guest run commands are documented in
``Documentation/mm/swmmu-kselftest-build.rst``.

Run the resulting UML kernel with a root filesystem containing the installed
tests. For example, when the root filesystem is at ``rootfs``:

.. code-block:: sh

   ./build/vmlinux root=/dev/root rootflags="$PWD/rootfs" \
           rootfstype=hostfs rw mem=2g loglevel=8 zpoline=1 init=/sbin/init

The root filesystem's init script is responsible for invoking the installed
kselftests. The configuration above enables the SWMMU KUnit test as well;
KUnit results are reported during kernel boot. Tests that rely on unsupported
occupied-target ``MREMAP_FIXED`` behavior are currently skipped as described
above.
