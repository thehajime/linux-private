====================
SWMMU GCC plugin
====================

This document describes the GCC plugin implemented in
``scripts/gcc-plugins/swmmu_plugin.c``. It is a compiler-side implementation
contract, including the conservative ``all-access`` mode and its experimental
the two build-wide access profiles and the fallback helper path. User-visible
SWMMU behavior and kernel interfaces are documented in
``Documentation/mm/swmmu.rst``.

Overview
========

The plugin instruments GIMPLE memory operations with calls to userspace
runtime helpers. The runtime must provide the helper ABI expected by the
plugin, and the selected kernel/runtime profile must support those operations.
The plugin does not inspect a process's VMAs or ask the kernel whether a
pointer is native or SWMMU-backed. It only uses compiler-visible pointer types, known allocator calls, and the direct-window
contract described below.

Build-wide profile
==================

A package can opt in using the plugin CFLAGS alone, with the matching SWMMU
runtime and headers installed in the toolchain sysroot::

   -include /path/to/swmmu-runtime-decls.h \
   -fplugin=/path/to/swmmu_plugin.so \
   -fplugin-arg-swmmu_plugin-all-access=1

The forced include must declare the runtime load/store functions with the
signatures expected by the plugin. The UAPI ``linux/nommu_swmmu.h`` alone
only declares flags and is insufficient. No source filename, directory,
allocator name, or application-specific pointer annotation is needed. The
kernel dynamic helpers dispatch each access
using the current VMA. For scalar accesses contained in one page, the kernel
checks the VMA and copies in one traversal; accesses crossing a page retain
preflight validation before any write. The userspace runtime must actually
call these helpers for uncertain pointers: a low-address direct-access
shortcut can bypass the kernel check after fork.

This build profile does not yet make mixed native/SWMMU fork a general
production configuration. The fork path may convert a native stack to SWMMU
at the same address while UML refuses a host alias that would overlap its own
physical-memory mapping. The plugin does not transform compiler-generated
prologue/epilogue stack accesses or assembly instructions. Those native CPU
accesses cannot follow an aliasless SWMMU stack. Before enabling default-off
SWMMU for arbitrary fork-using programs, the kernel and compiler must provide
a directly accessible stack across fork or otherwise handle every such access.

This profile retains a syscall for an access whose pointer cannot be proved
native by the compiler. Its speed must be measured on the deployed workload.
The plugin intentionally has no numeric native-range, source-file, source-
directory, or allocator-name switches. A numerical address alone is not a
durable native VMA property: a native private mapping can be converted to an
SWMMU VMA at fork without changing its address, and an SWMMU VMA may lack a
host alias where it intersects UML's reserved host range. A general direct-
access optimization would need kernel-maintained, per-mm eligibility state
that stays current across mapping changes and mm activation. The reserved
alias window described below provides a narrower backend guarantee.

All-access mode
===============

Pass ``-fplugin-arg-swmmu_plugin-all-access=1`` to instrument the eligible
memory operations in every compiled function, apart from plugin runtime
functions. This is the conservative
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

Access decision flow
====================

The names used by the compiler and kernel describe different decisions.
``all-access`` selects which compiler-visible accesses are covered. It does
not mean that every covered access necessarily executes a helper. A covered
access can still take a compiler-proven or kernel-authorized direct path::

   compiler-visible memory operation
                  |
                  v
       eligible for instrumentation? ---- no ---> ordinary native access
                  |
                 yes
                  |
                  v
       compiler proves native storage? --- yes --> ordinary native access
                  |
                  no
                  |
                  v
       direct-window guard authorizes
       this complete access? ------------- yes --> ordinary native access
                  |
                  no
                  |
                  v
          dynamic userspace helper
                  |
                  v
       runtime can use a guaranteed
       native path? ---------------------- yes --> ordinary native access
                  |
                  no
                  |
                  v
       kernel SWMMU load/store helper
                  |
                  v
       validate current mm and VMA, then access backing storage

The production-oriented build-wide profile uses ``all-access`` coverage with
the ``direct-window`` guard and dynamic helpers as the fallback. ``post-scalar``
and ``return-value-load`` remain separate compiler/runtime experiments for
measurement; neither is needed for the standard profile.

Terminology
===========

``all-access``
   A compiler coverage policy. The plugin considers eligible GIMPLE memory
   operations throughout every compiled function. Direct paths can still
   remove helper calls after an access has entered this coverage.

``selective``
   The compiler policy used without ``all-access``. Only annotated functions,
   pointer types, and supported annotated operations are transformed. It
   requires application-specific pointer-boundary information.

``dynamic helper``
   The fallback route for a pointer whose storage cannot be proved native by
   the compiler or authorized by a direct guard. The runtime and kernel decide
   from the current process mapping whether native or SWMMU backing applies.

``direct window``
   A kernel-authorized, immutable host alias interval. An inline guard permits
   ordinary CPU loads and stores only when the complete access lies in the
   published interval. It is a backend contract, rather than a conclusion
   drawn from an application's source layout.

``default-on``
   The kernel mapping policy in which eligible NOMMU mappings start under
   SWMMU management. This is independent of whether the plugin was compiled in
   all-access or selective mode.

``default-off``
   The experimental kernel mapping policy in which eligible mappings start as
   native and may need conversion when process semantics such as private COW
   are required. The compiler policy does not make that conversion safe by
   itself.

``guarded lowering``
   Plugin lowering that emits an inline direct-access eligibility check and a
   dynamic-helper fallback for the same operation.

``post-scalar``
   Experimental pass placement that leaves ordinary GIMPLE accesses visible
   through GCC scalar optimization before lowering them to helpers. It changes
   transformation order, not mapping policy.

``native proof``
   Compiler-visible provenance or an explicit build assertion that allows an
   access to bypass helpers. A numerical address alone is not a native proof,
   because fork or mapping operations can change the VMA without changing the
   pointer value.

Attributes
==========

``__attribute__((swmmu))``
   Marks a function as operating in SWMMU context for the selective pass.

``__attribute__((swmmu_ptr))``
   Type attribute for a pointer type that denotes an SWMMU pointer. It guides
   the selective pass and pointer-state propagation. It is not a runtime check.

``__attribute__((swmmu_native_ptr))``
   Internal type annotation used by the direct-window runtime wrappers to
   keep their implementation out of helper lowering. It is not an application
   optimization switch or a kernel-validated mapping guarantee.

``__attribute__((swmmu_allocator_result))``
   Marks a pointer-returning allocator for the selective pass so its result is
   treated as dynamically backed and uncertain accesses retain helper checks.
   It does not assert native storage.

``__attribute__((swmmu_memop("memcpy")))`` (also ``memmove`` and ``memset``)
   Marks a function declaration as the corresponding memory operation so the
   plugin can route it through the matching SWMMU helper when required.

Options
=======

``-fplugin-arg-swmmu_plugin-post-scalar=1``
   Experimental all-access lowering after GCC's ``pre`` pass, before loop
   vectorization. Scalar redundancy elimination and scalar replacement can run
   before accesses become opaque calls. Functions compiled at ``-O0`` retain
   the existing IPA lowering. The late GIMPLE pass must not recreate callgraph
   edges after GCC has released IPA summaries. This option cannot be combined
   with the guarded ``direct-window`` profile. It needs workload measurements;
   fewer calls in a loop do not guarantee a faster complete application.

``-fplugin-arg-swmmu_plugin-direct-window=1``
   Select the early, inline guarded lowering using a matching runtime header
   that reads kernel-authorized bounds. The reference header is
   ``scripts/gcc-plugins/tests/swmmu_direct_window_guard.h``. It uses no source
   lists or application-specific address constants. After a successful
   ``NOMMU_SWMMU_QUERY_DIRECT_WINDOW`` query, the runtime selects a page-aligned
   power-of-two subset of the returned interval. It selects a key
   whose low six bits encode the address shift and whose upper bits encode
   the address prefix. The shift must be at least 12, and the prefix must be
   at least 2 and fit in 58 bits. The matching libc publishes the word
   ``nommu_swmmu_direct_key`` atomically after querying the kernel. The initial
   value ``UINTPTR_MAX`` matches no 64-bit address. Cached initial snapshots
   retain helper fallback. Even mixed initial/valid prefix and shift fields
   cannot match an address under these encoding constraints.

   The reference header calls compiler-only const intrinsics
   ``__swmmu_direct_key_snapshot`` and ``__swmmu_direct_key_snapshot_libc``.
   GCC can reuse these queries through scalar optimization. A dedicated pass
   after ``optimized`` replaces them with native key reads before RTL
   expansion, removing runtime call overhead. They must not remain undefined
   symbols in a linked program. The key certifies immutable geometry, not
   mutable VMA permissions. See the reserved alias-window contract in
   ``swmmu.rst``. This is an experimental backend-specific contract: public
   key storage must not be modified by applications.

   Guarded lowering runs after GCC's early local scalar optimization and
   before IPA inlining. This leaves ordinary memory operations visible to
   early forwarding, scalar replacement, and redundant-access elimination,
   while still allowing the always-inline guard wrappers to disappear before
   code generation. This ordering applies to the direct-window profile; it does not change volatile handling or
   the dynamic fallback rules.

   On the currently supported x86_64 UML backend, scalar accesses entirely
   within one 4 KiB page and this subset execute ordinary CPU instructions.
   For 1, 2, 4, and 8 byte operations whose GIMPLE memory reference has at
   least the corresponding power-of-two alignment, the compiler omits the
   explicit page-boundary test: such an operation cannot cross a page because
   the page size is a multiple of its alignment. Packed, under-aligned,
   volatile, and otherwise uncertain references retain the full guarded or
   dynamic path.
   The inline assembly describes the accessed bytes with precise, unaligned,
   alias-permitting memory operands. Ordinary accesses do not use a global
   memory clobber, so unrelated memory operations can still be optimized.
   Volatile accesses retain opaque helpers to preserve repeated reads and
   writes. Other accesses retain the dynamic helper path. The matching libc
   must export ``nommu_swmmu_direct_key`` with the publication contract above.
   The key can cover less than the complete reservation; accesses to the
   remainder use the runtime fallback. The public intrinsic uses an explicit
   GOT reference, avoiding executable data copy relocations. During libc
   bootstrap, ``SWMMU_BUILDING_LIBC`` selects a hidden RIP-relative read.
   Ordinary CRT objects use the public reference; static PIE's ``rcrt1.o``
   requires ``SWMMU_STATIC_PIE_CRT`` to use the hidden reference before
   self-relocation. Package CFLAGS can be supplied by one common wrapper;
   keep the existing ``--no-relax`` link workaround::

      -include /path/to/swmmu-runtime-decls.h \
      -include /path/to/swmmu_direct_window_guard.h \
      -fplugin=/path/to/swmmu_plugin.so \
      -fplugin-arg-swmmu_plugin-all-access=1 \
      -fplugin-arg-swmmu_plugin-direct-window=1

   The default-off native-to-SWMMU fork limitation described above still
   applies outside this window. This option does not certify native physical
   addresses or resolve aliasless native stack migration.

``-fplugin-arg-swmmu_plugin-return-value-load=1``
   In all-access mode, lower scalar loads to
   ``nommu_swmmu_load_return_value``. This helper must be provided by the
   runtime and must preserve arbitrary data values, including values in the
   syscall errno range. Stores and bulk operations retain their regular helper
   interfaces. This changes the helper ABI and must match the runtime build.

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
For release-facing behavior and the userspace runtime ABI, see
``Documentation/mm/swmmu.rst``.
