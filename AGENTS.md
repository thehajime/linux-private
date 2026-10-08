# SWMMU project handoff

This file is the concise, current handoff for SWMMU/NOMMU development. Keep
release-facing feature descriptions in `Documentation/mm/swmmu.rst`, and keep
reproducible kselftest build instructions in
`Documentation/mm/swmmu-kselftest-build.rst`.

## Project status

- **Current phase:** cleanup and validation before feature development.
- **Handoff and CI branch:** `ci/uml-nommu` (the default branch and the only
  branch on which this ledger is maintained).
- **Implementation branch:** `feature-swmmu-nommu`; do not maintain a second,
  independently edited AGENTS.md there.
- **Current profile:** experimental paged SWMMU on UML with `CONFIG_MMU=n`,
  `CONFIG_NOMMU_SWMMU=y`, and `CONFIG_UML_NOMMU_SAS=y`.
- **Correctness baseline:** SVM-style all-access instrumentation with raw
  pointers and runtime address translation.
- **Userspace checkpoint:** instrumented Bash and a short lmbench smoke have
  completed. Coreutils is the next package-profile target; nginx, OpenSSL, and
  broader application validation have not started.
- **Next validation task:** build a fresh kselftest tree using the documented
  workflow, run both `zpoline=0` and `zpoline=1`, and record exact KUnit and
  kselftest TAP totals.

## Whole milestones

- [x] Establish the all-access compiler/runtime model.
- [x] Bring up paged SWMMU startup and instrumented userspace.
- [x] Implement and validate SAS aliases for the active `mm_struct`.
- [x] Exercise the reported KUnit, compiler-plugin, UML kselftest, Bash, and
  lmbench smoke-test profiles.
- [x] Complete the first source-cleanup pass: VMA clone preparation,
  transaction state, resize locking, alias teardown, and plugin organization.
- [x] Document a reproducible fresh kselftest build.
- [x] Move the authoritative handoff and CI workflows to `ci/uml-nommu`.
- [ ] Reproduce the fresh kselftest build and record exact results for both
  zpoline modes. **This is the next task.**
- [ ] Close the remaining correctness gaps for signals, mappings, user-copy,
  nonlocal control flow, and supported compiler-generated accesses.
- [ ] Extend the instrumented Alpine profile through coreutils, then validate
  nginx, OpenSSL, and broader applications. **Nginx is not started.**
- [ ] Define the final mode ABI and expand beyond the current x86_64 SAS scope.
- [ ] Address concurrency and performance after functional validation is
  stable.

## Active TODOs

### Immediate

- [ ] Build the disposable kselftest tree described in
  `Documentation/mm/swmmu-kselftest-build.rst`.
- [ ] Run the installed suite with `zpoline=0` and `zpoline=1`; record the
  configuration, exact TAP totals, skips, and any intermittent failure.
- [ ] If the fresh-build parser panic recurs, save the full boot log and retry
  under GDB. Record the failing task, `mm`, fault address, instruction pointer,
  and zpoline/seccomp mode; do not present the uninstrumented-Python theory as
  established fact.
- [ ] Continue cleanup as small, reviewable changes before adding features.

### Correctness and coverage

- [ ] Finish the user-copy architecture and remaining aggregate/bitfield
  read-modify-write coverage.
- [ ] Define exception and interrupt behavior and complete signal-frame,
  `sigreturn()`, restart, pipeline, and `/sbin/init` lifecycle validation.
- [ ] Complete `setjmp()`/`longjmp()` and
  `sigsetjmp()`/`siglongjmp()` integration.
- [ ] Cover the remaining mapping cases: general file-backed and `MAP_SHARED`
  mappings, device mappings, partial `mprotect()`, complete executable
  permission propagation, and raw copy-from-user.
- [ ] Cover compiler/runtime gaps including spills/reloads, stack behavior,
  TLS/context switching, atomics, volatile, vector/floating-point accesses,
  inline assembly, and uninstrumented libraries.
- [ ] Add remap expansion rollback tests using existing fault-injection
  facilities; do not add production-only test hooks.
- [ ] Implement occupied-target `MREMAP_FIXED` transactionally for MMU and
  SWMMU in a separate patchset, then enable its currently skipped test.
- [ ] Revisit `MREMAP_DONTUNMAP` and validate `CONFIG_MMU=y` separately so
  pre-existing MMU failures are not attributed to SWMMU.

### ABI, platform, and userspace

- [ ] Audit whether provisional `nommu_swmmu_alloc()` and
  `nommu_swmmu_free()` interfaces remain. If they do, remove their syscall
  plumbing in favor of standard `mmap()`, `munmap()`, and `mremap()` while
  keeping compiler allocator contracts separate.
- [ ] Define explicit legacy, flat, and paged mode values and their transition
  from the provisional `PR_SET_SWMMU`/`PR_GET_SWMMU` interface; reassess
  `MAP_NOMMU_SWMMU_FLAT` after that design is settled.
- [ ] Synchronize aliases through the UML runner mapping path before claiming
  non-SAS support.
- [ ] Audit NOMMU SAS signal object selection, `is_user` propagation, and the
  relay paths; validate SIGABRT, SIGSEGV, and SIGBUS delivery and termination.
- [ ] Determine whether the current musl profile needs `brk()`; implement
  paged-mode support or keep the affected allocator configuration disabled.
- [ ] Extend the reproducible Alpine SWMMU package/rootfs profile through
  coreutils, the next intended package target.
- [ ] After the required package closure exists, begin nginx and OpenSSL
  validation. Neither is currently validated or in progress.
- [ ] Evaluate ARM and ARM64 only after the x86_64 profile is stable.

### Optimization and performance (postponed)

- [ ] Treat pointer-state analysis, native-access selection, explicit opaque
  API contracts, interprocedural summaries, and LTO as optional optimization,
  not correctness requirements.
- [ ] Make lazy zpoline patching safe for concurrent execution/instruction
  fetch, or define a safe fallback policy.
- [ ] Revisit the high `lat_proc shell` result and SAS-seccomp slowdown only
  after functional validation is stable.

### CI, packaging, and delivery

- [ ] Complete plain NOMMU CI validation. Runs `37711748667` and
  `37713808402` showed that test-0's `NOMMU_USERSPACE_READY` marker could print
  after `/bin/mkdir` failed to load shared libraries and `/dev/shm` and
  `/dev/pts` mounts failed; the prior readiness check was a false positive.
  test-1 also regenerated its config without reapplying
  `CONFIG_NOMMU_SWMMU=n`, then panicked with init `-EFAULT`. The workflow now
  reapplies the profile config in build-1, and rcS stops on failed setup
  commands. Run `37725137250` passed both UML tests with coreutils omitted,
  but benchmark-1 still logged `lat_select` failing to load `libtirpc.so.3`.
  Plain NOMMU's `musl-nommu` split rebuild lacked `DL_NOMMU_SUPPORT=1`, so
  musl's loader treated NOMMU `MAP_FIXED` behavior as a fatal shared-library
  mapping error. The aports fix is committed locally as
  `f9ec9629f50634534a0ee89339ca5ed5633aa503` and the CI APK source pin is
  updated. A local APK build and UML smoke with coreutils installed completed:
  `/bin/mkdir` and lmbench `lat_select file 10` ran without loader errors.
  The first push run `37739792071` stopped in APK setup because push-triggered
  builds checked out `ci/uml-nommu` as the kernel source, which lacks the SWMMU
  plugin and UAPI. The workflow now defaults push-triggered validation to
  `feature-swmmu-nommu`; rerun before closing plain NOMMU validation.
  The earlier plain NOMMU kernel fixups are feature commits
  `d746ed04614e` and `02036e380729`.
- [ ] Re-run RISC-V LTP build after fixing the install archive path. Run
  `37701275797` built and installed LTP successfully, then failed because the
  helper archived `/github/home/ltp-install` while the Buildroot container
  installed under `/root/ltp-install`. The archive now uses the actual install
  path; run `37707094706` was cancelled after its kernel ref checkout failed.
- [ ] Remove the temporary LTP skips for `ptrace06` on `um-mmu` and
  `perf_event_open03` on `um-nommu-seccomp` and `um-nommu-sas-seccomp` after
  their failure causes are understood and fixed.
- [ ] Validate the three UML userspace profiles in CI: Alpine standard for
  MMU, plain `SWMMU=0` NOMMU APKs for non-SAS UML, and instrumented `SWMMU=1`
  NOMMU APKs for NOMMU-SAS. The SWMMU plugin and UAPI must come from the same
  kernel ref tested by the workflow.
- [ ] Run the GitHub Actions UML build/test matrix for the updated
  `feature-swmmu-nommu` revision `a4de425be710`. Run `37551053113` was
  cancelled before the UML matrix started; its NOMMU APK build and artifact
  upload completed, while the local RISC-V image build was interrupted.
  Record final outcomes and address new failures.
- [ ] Continue the LTP matrix after run `37770276697`. Checkpatch, APK builds,
  both standard LTP build jobs, all four UML build/KUnit jobs, and all six
  benchmark jobs passed. The RISC-V Buildroot image build (including the
  `qemu-system-misc` install) succeeded, and its 4.1 GB artifact uploaded, but
  the Buildroot job was cancelled after exceeding its 100-minute timeout during
  post-build cleanup. This caused the RISC-V LTP build and all LTP test jobs to
  be skipped. The job timeout is now 150 minutes; rerun and verify the cached
  Buildroot image and the LTP matrix. NOMMU-SAS LTP jobs in run `37767571411`
  exited 137 at the first
  `mkdir`: host-side preparation commands still resolved to NOMMU BusyBox
  applets. The direct host shell now uses a separate BusyBox applet path, and
  `faketty` runs through the host shell so its `/bin/sh` shebang cannot select
  the NOMMU shell. Verify both with the next full run. Plain NOMMU completed its
  tests but artifact collection failed on guest-owned files; a runner-side
  `chown` now restores access. RISC-V LTP failed because
  `qemu-system-riscv64` was missing; the local image now installs
  `qemu-system-misc`, with `apt-get update` in the same layer to avoid stale
  package-index 404 errors seen in run `37769854083`. NOMMU-seccomp hit a kernel
  panic in `epoll_wait01` through `rb_erase_linked()` / `__remove_hrtimer()`;
  investigate this separately and keep the failure visible. The previous run
  `37747988970` also exposed
  that native/MMU images lack `/tmp/busybox-host`, that runtime `apk add`
  segfaults after replacing musl, and that LTP support files should be checked
  out on the runner. Those issues were addressed by runner-side shell selection,
  build-time runtime dependencies, and a runner-side LTP checkout. The native
  stock Alpine job installs its runtime packages before testing. The MMU panic
  in `vma_move_commit()` from run `37741458984` was fixed separately by
  explicit no-op move commit/abort callbacks on `feature-swmmu-nommu`.
- [ ] Continue the LTP matrix after run `37794627130`. Checkpatch, APK builds,
  all four UML build/KUnit jobs, both Alpine LTP build jobs, RISC-V image/LTP
  builds, all six benchmarks, and MMU, MMU-seccomp, native, and RISC-V LTP jobs
  completed successfully. The RISC-V LTP report contains 135 failed and 52
  broken tests; report test counts separately from the successful job status.
  Four NOMMU LTP jobs failed. The pre-extracted `/opt/ltp` bind mount fixed the
  archive extraction SIGKILL from the prior run. SAS LTP jobs now reach the
  runner but `/usr/bin/script` from the NOMMU image segfaults before guest boot;
  a local reproduction confirmed it is the image's util-linux binary. Use the
  Docker exec TTY directly and capture output on the runner, rather than calling
  that binary inside the image. Keep the NOMMU idle container alive with the
  static host BusyBox sleep applet; the image's `/bin/sleep` also segfaults when
  called outside UML. Plain NOMMU panicked in `epoll_wait01`; NOMMU-seccomp
  completed its LTP summary (6768 pass, 80 fail, 4 broken) then panicked while
  Python Kirk exited, with the last test failure `waitpid01` (return 137).
  `run-ltp-job.sh` now skips `epoll_wait01` on plain NOMMU and `waitpid01` on
  NOMMU-seccomp pending kernel fixes. Retest the SAS runner change and assess
  whether the NOMMU-seccomp exit panic is resolved; do not count successful
  benchmark runs as proof that LTP passed. Commit `c2a22d3a8d97` had already
  moved LTP archive extraction to the runner and bind-mounted `/opt/ltp`. Run
  `37843548583` exposed false-positive LTP job conclusions: SAS-zpoline panicked
  before producing results, while NOMMU-seccomp's guest Python segfaulted;
  both jobs showed success because the host step piped `docker exec` through
  `tee` without `pipefail`. Require a nonempty HTML result after NOMMU runs and
  propagate the Docker exit status before treating these jobs as successful.
- [x] Verify UML ccache restore and save. Run `37761713019` restored exact
  immutable keys such as `Linux-um-nommu-ccache-build`, so the cache action
  skipped its post-job save. Run `37767571411` then missed its old
  `Linux-um-mmu-tt-ccache-build` cache because the restore prefix had an extra
  trailing hyphen, but successfully saved
  `Linux-um-mmu-tt-ccache-build-37767571411-1`. The key includes `github.run_id`
  and `github.run_attempt`; the restore prefix now also matches the legacy key.
  Run `37770276697` restored the previous cache and saved a new run-specific key
  for each of the four UML profiles. The ccache entries currently occupy about
  6.7 GB total; monitor GitHub cache eviction and storage as future runs add
  immutable per-run entries.
- [ ] Verify BusyBox `cd -` in the generated NOMMU binary before publishing an
  image.
- [ ] Upstream the `busybox-nommu` and `musl-nommu` APKBUILD changes to Alpine.
- [ ] Keep ordinary CI image builds local. Publish the stable
  `3.20.3-um-nommu` image only after the UML matrix succeeds and by explicit
  dispatch; record kernel/aports revisions and the image digest.
- [ ] Propose the UML `.lbss` linker-script placement fix upstream after
  validating static and dynamic UML builds against the `uml/next` base.
- [ ] Implement and compare the full SWMMU/non-SWMMU benchmark suite: lmbench,
  getpid, iperf3, netperf, and LTP.
- [ ] Publish benchmark results in a durable, reviewable location. Investigate
  embedding the GitHub Pages report or its charts in the results issue.
- [ ] Group “Changes from previous report” by benchmark family and distinguish
  missing samples from regressions or benchmarks that did not run.
- [ ] After the updated feature branch no longer needs its staging ref, rename
  `codex/feature-swmmu-nommu-followup` to a clearly marked
  `to-be-deleted-*` branch before deletion. Keep the pre-update backup ref
  until rollback is no longer needed.

## Project policy

### Sources of truth

Use sources in this order:

1. Current tracked implementation and tests.
2. This file for active status, policy, and task ownership.
3. `Documentation/mm/swmmu.rst` on `feature-swmmu-nommu` for release-facing
   feature behavior and user-visible constraints, not development progress or
   test diaries.
4. `Documentation/mm/swmmu-kselftest-build.rst` on
   `feature-swmmu-nommu` for the fresh test-build procedure.

Older chats, the removed `Documentation/mm/swmmu-status.md`, historical
appendices, and experimental branches may be stale. When they disagree with
tracked code, verify the implementation before updating this handoff.

### Development policy

- Keep `.github/workflows` and this handoff on `ci/uml-nommu`; keep upstream
  implementation patches and release-facing SWMMU documentation on the
  feature branch. Update this ledger when feature-branch milestones change.
- Keep changes upstreamable and reviewable. Prefer small NOMMU/SWMMU-specific
  changes and reuse common MM infrastructure where it naturally fits.
- Do not change broad MMU behavior solely to simplify SWMMU.
- Keep SWMMU pagetable preparation and rollback isolated unless an abstraction
  is demonstrably shared with generic MM code.
- Preserve raw pointer representation and dynamic translation as the
  correctness fallback. Do not revive pointer-provenance or mallocng-specific
  propagation as correctness machinery.
- Do not add application-owned SWMMU annotations as a substitute for the
  all-access correctness baseline.
- Leave unrelated tracked or untracked changes alone. Do not change
  `.gitignore` unless explicitly requested.
- Do not make support claims from a boot or smoke test. State the exact mode,
  configuration, suite, pass count, and skip count.

### Validation workflow

Do not add or run tests unless the task requests validation. Prefer an existing
configured output tree for implementation checks. For a fresh kernel output:

```sh
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
```

Use a separate output directory for fresh kselftests as documented in
`Documentation/mm/swmmu-kselftest-build.rst` on `feature-swmmu-nommu`. The
reported UML command is:

```sh
./build/vmlinux root=/dev/root rootflags="$PWD/rootfs" \
        rootfstype=hostfs rw mem=2g loglevel=8 zpoline=1 init=/sbin/init
```

Use `zpoline=0` for SAS-seccomp mode. Record exact KUnit and kselftest TAP
totals and the tested configuration. A boot or smoke test is not equivalent to
the full kselftest suite.

For broader validation, preserve this order: KUnit and compiler regression
suites; SAS default-on kselftests; instrumented musl/loader/BusyBox startup;
focused alias, mapping, signal, and TLS probes; Bash and lmbench smoke;
coreutils package and command validation; `/sbin/init` lifecycle; then nginx
and broader applications. Do not skip package dependencies merely to reach a
later application milestone.

### Handoff maintenance

- Mark a task complete only when source and relevant validation support it.
- Move completed work out of active TODOs into **Completed TODOs**.
- Keep active next steps separate from deliberately postponed work.
- Put lasting technical conclusions in `Documentation/mm/swmmu.rst`; leave
  only a concise operational summary here.
- Record intermittent failures as intermittent and separate evidence from
  hypotheses.
- Report the checkout, branch, starting and ending HEAD, preserved local
  changes, validation performed, unperformed work, push target, and pushed SHA.
- Keep this Markdown ledger as the task source of truth while work is centered
  on one branch. Consider GitHub Projects only when cross-repository work,
  multiple owners, or scheduling/views justify the extra synchronization; if
  adopted, link issues here rather than duplicating technical status.

## Completed TODOs

- [x] Cover scalar, aggregate, array, alias, cast, PHI, cross-function, and
  unknown-return accesses in the compiler plugin, including memory builtins
  and the tested dynamic bitfield and atomic cases.
- [x] Bring up default-on paged SWMMU startup, including initial exec stack,
  argument/environment setup, and FDPIC stack relocation.
- [x] Implement SAS host aliases and active-`mm_struct` switching; preserve
  private-page behavior and prevent inactive teardown from removing active
  aliases.
- [x] Add supported SWMMU-aware copy-to-user and `clear_user()` paths plus
  full-VMA `mprotect()` alias-permission updates.
- [x] Route supported private-copy FDPIC executable mappings through SWMMU and
  execute through SAS aliases.
- [x] Validate instrumented musl/loader startup, BusyBox through `rcS`, focused
  TLS/signal-return and vfork/exec/waitpid probes, and the reported KUnit and
  kselftest profiles.
- [x] Restore the normal x86 syscall rewind for the tested restart path.
- [x] Run the reported Bash and short lmbench smoke tests.
- [x] Factor common VMA clone setup while leaving SWMMU pagetable preparation
  and rollback specific to SWMMU.
- [x] Review transaction callbacks; retain separate prepare/commit/abort
  families because no useful common abstraction was found.
- [x] Remove unused transaction state and fix resize-range release locking.
- [x] Reorganize the GCC plugin source without intended behavior changes.
- [x] Replace the former status document with this handoff and a reproducible
  fresh-kselftest build document.
- [x] Build signed `busybox-nommu` and `musl-nommu` APKs from a pinned aports
  revision in GitHub CI, verify their signatures in the UML rootfs image, and
  remove the expiring GitLab artifact dependency.

## Technical memorandum

### Current design invariants

- Pointers keep their normal raw representation. Supported source-level
  accesses are instrumented and runtime helpers translate addresses.
- SWMMU virtual ranges live in `mm->mm_mt`; backing pages are held by SWMMU
  page tables.
- SAS host aliases represent only the active SWMMU `mm_struct` in UML's shared
  host address space. Task/mm switches must replace aliases; updates to an
  inactive mm must defer fixed host-alias operations.
- The SAS physical-memory mapping and aliases must share backing storage. Do
  not confuse SWMMU guest ranges with UML kernel physical memory or
  `uml_to_phys()`/`uml_to_virt()`.
- The separate non-SAS UML runner is not supported by the current alias
  implementation.

### Known limitations

- Lazy SIGSYS zpoline patching rewrites the two-byte syscall instruction from
  the host-reported continuation address. It is tested only in the recorded
  single-threaded scope and is unsafe against concurrent execution/fetch.
- Paged mode requires the loader, libc, programs, and relevant libraries to be
  built for SWMMU. This applies to static and dynamic binaries; rebuilding only
  musl is insufficient. Incidental success of an uninstrumented binary is not
  a compatibility guarantee.
- Direct executable file mappings, shared/device mappings, partial protection
  changes, and the compiler/runtime cases listed in active TODOs remain
  incomplete or profile-specific.
- Occupied-target `MREMAP_FIXED` is unsupported because the destination cannot
  yet be restored transactionally after a failed move. Empty-target behavior
  remains supported.
- The current mode ABI is provisional. Flat mode, mixed native/SWMMU VMA fork,
  non-SAS UML, ARM/ARM64, and broad application coverage are deferred.
- `vfork()` is supported only for its narrow `vfork()`-then-`execve()` use; it
  is not a general compatibility mechanism.

### Recorded validation baseline

The branch has recorded successful SWMMU KUnit, compiler-plugin, repeated UML
NOMMU kselftest, Bash smoke, and short lmbench smoke runs for their stated
configurations. After the inactive-mm alias teardown guard, focused runs for
both zpoline modes reported guest mmap **6 pass / 1 skip**, SWMMU **37 pass / 7
skip**, dedicated SWMMU KUnit **27 pass / 1 skip**, and UML register suite **1
pass**. `nommu_mremap_test` was excluded because it was separate draft work.
These figures are not broad application or complete official-runner coverage.

The instrumented `/bin/bash.instrumented` passed `/bin/sh` POSIX-mode and
`--noprofile --norc` smoke tests covering loops, functions, command
substitution, and a pipeline. The short lmbench smoke completed `lat_select`,
syscall/file-operation tests, `lat_proc shell`, and `lat_proc exec`. These are
smoke results, not full Bash or performance validation. The high
`lat_proc shell` result predates this SWMMU work and remains deferred.

An observed uninstrumented coreutils `id` success is incidental and does not
establish compatibility for uninstrumented paged-SWMMU applications. The
planned package sequence is coreutils first and nginx later; nginx and OpenSSL
have not been started.

### SAS alias teardown development record (2026-10-05)

- Teardown of an inactive mm must clear that mm's range-mapping state without
  unmapping host aliases at the same addresses when another mm is active.
  Physical cleanup may proceed when the active mm is `NULL` or owns the range.
- Without the guard, delayed child teardown after exit or exec could remove
  the parent's active aliases. The old minimal rootfs reproduced the resulting
  stack fault under GDB in SAS-seccomp mode with mremap tests excluded. Normal
  execution was timing-sensitive; Alpine and minimal rootfs used identical
  instrumented musl and SWMMU binaries.
- After the guard, focused `zpoline=0` and `zpoline=1` runs reported guest mmap
  **6 pass / 1 skip**, SWMMU **37 pass / 7 skip**, dedicated SWMMU KUnit
  **27 pass / 1 skip**, and UML register suite **1 pass**.
- `nommu_mremap_test` was excluded because it remained separate draft work;
  the test was not removed. These were focused results, not complete
  official-runner or broad application coverage.
- The cloud `zpoline=1` run used a scoped Docker workload with
  `CAP_SYS_RAWIO` for address-zero mapping while host `mmap_min_addr` remained
  4096. `zpoline=0` did not need that capability. The `is_user` audit remains
  deferred.

## Appendix: references

- `feature-swmmu-nommu:Documentation/mm/swmmu.rst` — release-facing SWMMU
  feature behavior and user-visible constraints; do not put development status
  or test diaries here.
- `feature-swmmu-nommu:Documentation/mm/swmmu-kselftest-build.rst` —
  reproducible disposable kselftest build and install flow.
- `feature-swmmu-nommu:tools/testing/selftests/mm/nommu` — SWMMU/NOMMU
  selftests.
- `aports`, branch `nommu-uml` — planned broader instrumented Alpine profile.
