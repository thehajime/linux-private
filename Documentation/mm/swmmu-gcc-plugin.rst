====================
SWMMU GCC plugin
====================

This document describes the GCC plugin implemented in
``scripts/gcc-plugins/swmmu_plugin.c``. It is a compiler-side implementation
contract, including the conservative ``all-access`` mode and its experimental
native-access options. User-visible SWMMU behavior and kernel interfaces are
documented in ``Documentation/mm/swmmu.rst``.

Overview
========

The plugin instruments GIMPLE memory operations with calls to userspace
runtime helpers. The runtime must provide the helper ABI expected by the
plugin, and the selected kernel/runtime profile must support those operations.
The plugin does not inspect a process's VMAs or ask the kernel whether a
pointer is native or SWMMU-backed. It only uses compiler-visible pointer types,
known allocator calls, local-variable addresses, and explicit build-time
assertions described below.

All-access mode
===============

Pass ``-fplugin-arg-swmmu_plugin-all-access=1`` to instrument the eligible
memory operations in every compiled function (apart from plugin runtime
functions and functions explicitly excluded by name). This is the conservative
correctness baseline for code that may access SWMMU memory through ordinary,
unknown, aliased, or cross-function pointers. Accesses not proven native go
through dynamic SWMMU helper calls. The generated path covers scalar loads and
stores, supported bitfields and atomics, aggregate copies/zeroing, and memory
builtins such as ``memcpy``, ``memmove``, and ``memset``. Exact support and
regressions are tracked by ``scripts/gcc-plugins/tests``.

All-access mode is a compile-time access policy, not a VMA classifier. An
ordinary pointer is not assumed to be native because its numerical address
looks low, and a mapping cannot be marked native just by choosing an address.
Unknown or mixed pointer states retain helper lowering. Build all userspace
code that can touch SWMMU memory with a compatible plugin/runtime combination;
uncompiled code and inline or assembly accesses outside the instrumented GIMPLE
operations are not covered by this transformation.

Without ``all-access``, the plugin uses its selective pass. It transforms
functions marked with ``__attribute__((swmmu))`` and accesses described by
``swmmu_ptr`` pointer types and supported annotated memory operations. Calls
across SWMMU pointer boundaries are checked; unknown pointer state at an
unannotated call or dereference is diagnosed rather than silently assumed safe.
This mode requires the application to identify SWMMU pointer boundaries
correctly and is not an equivalent drop-in safety baseline for arbitrary
ordinary pointers.

Attributes
==========

``__attribute__((swmmu))``
   Marks a function as operating in SWMMU context for the selective pass.

``__attribute__((swmmu_ptr))``
   Type attribute for a pointer type that denotes an SWMMU pointer. It guides
   the selective pass and pointer-state propagation. It is not a runtime check.

``__attribute__((swmmu_native_ptr))``
   Type attribute asserting that a pointer always refers to memory accessible
   with native CPU loads and stores. Under all-access instrumentation,
   dereferences through this type can bypass helpers. Aliases and pointer
   arithmetic retain the state; incompatible control-flow merges fall back to
   helper lowering. Loading the pointer field itself from SWMMU memory still
   uses a helper. The promise is supplied by the build and is not validated
   against mappings, remapping, allocator reuse, fork, or library boundaries.

``__attribute__((swmmu_allocator_result))``
   Function attribute asserting that a pointer-returning function returns
   native memory. It is used only with native allocator-result mode. The
   compiler does not verify the function implementation or lifetime.

``__attribute__((swmmu_memop("memcpy")))`` (also ``memmove`` and ``memset``)
   Marks a function declaration as the corresponding memory operation so the
   plugin can route it through the matching SWMMU helper when required.

Options
=======

``-fplugin-arg-swmmu_plugin-return-value-load=1``
   In all-access mode, lower scalar loads to
   ``nommu_swmmu_load_return_value``. This helper must be provided by the
   runtime and must preserve arbitrary data values, including values in the
   syscall errno range. Stores and bulk operations retain their regular helper
   interfaces. This changes the helper ABI and must match the runtime build.

``-fplugin-arg-swmmu_plugin-native-stack=1``
   In all-access mode, treat direct references to automatic local variables
   and pointers derived from their addresses as native. Other pointer accesses
   retain helper lowering unless another native contract applies. This is an
   experimental assumption about stack mappings, not proof from the kernel.
   It does not cover stack accesses emitted after the plugin's GIMPLE pass,
   such as target stack-clash probes, or establish that escaped pointers remain
   valid across stack switching and process creation.

``-fplugin-arg-swmmu_plugin-native-allocator-results=1``
   Treat results of ``malloc``, ``calloc``, and ``realloc`` as native, with
   ``realloc`` native only when its input is already native. Also applies to
   functions declared with ``swmmu_allocator_result``. This is an assertion
   about every selected allocator result in that build profile.

``-fplugin-arg-swmmu_plugin-native-allocator-function=NAME``
   Treat a pointer result from the named function as native. Repeat for each
   allocator function.

``-fplugin-arg-swmmu_plugin-native-reallocator-function=NAME``
   Treat a result from the named reallocator as native only when its input
   pointer is already known native. Repeat for each reallocator function.

``-fplugin-arg-swmmu_plugin-native-source-file=SUFFIX``
   Treat every function whose source filename equals ``SUFFIX`` or ends in
   ``/SUFFIX`` as native and leave its memory accesses uninstrumented. Repeat
   the option for each source file. This is a narrow build-profile assertion
   intended for source units whose complete pointer input and allocation
   closure is known to remain native. It does not inspect the functions,
   pointers, VMAs, callbacks, or headers included by the source file.

``-fplugin-arg-swmmu_plugin-native-source-directory=NAME``
   Apply the same assertion to functions whose source path contains ``NAME``
   as a complete directory component. This also covers inline functions whose
   declarations come from headers in that directory. Directory matching is
   intentionally explicit and does not infer that included code is native.

``-fplugin-arg-swmmu_plugin-native-range-guard``
   Lower scalar loads and stores through the always-inline
   ``nommu_swmmu_load_dynamic_guarded`` and
   ``nommu_swmmu_store_dynamic_guarded`` runtime entry points. The runtime
   header must define these wrappers and directly access only an address range
   that the kernel guarantees is native; all other addresses must fall back to
   the dynamic helpers. This option runs before the normal optimization passes
   so the wrappers and their native branches can be inlined and optimized.
   It cannot be combined with ``return-value-load``.

The allocator options propagate known native state through aliases. A merge
with a SWMMU, ordinary, or dynamic pointer does not make the merged pointer
native. The options cannot detect allocator replacement, custom allocator
hooks, address-space changes, or fork semantics. Do not enable them unless the
complete allocator family and target runtime guarantee native mappings for the
full pointer lifetime.

``-fplugin-arg-swmmu_plugin-exclude-function=NAME``
   Exclude the named function from plugin transformation. This is suitable
   only for functions that do not require SWMMU access instrumentation, such as
   a verified runtime helper. Excluding a function that dereferences SWMMU
   memory bypasses the safety baseline. Repeat for each function name.

Test targets
============

The compiler regression tests check the emitted lowering and fallback paths.
With the plugin built at ``build-kselftest/swmmu_plugin.so``, focused targets
include:

.. code-block:: sh

   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" all-access
   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" all-access-return-value
   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" all-access-native-stack
   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" all-access-native-allocator-results

These tests do not validate the mapping promises made by native-pointer,
native-stack, allocator, or native-source-file options. Validate those
promises with the actual
userspace, kernel, fork/mapping behavior, and workload before relying on them.

For release-facing behavior and the userspace runtime ABI, see
``Documentation/mm/swmmu.rst``. For cross-building and running SWMMU selftests,
see ``Documentation/mm/swmmu-kselftest-build.rst``.
