# SWMMU project handoff

This file is the concise, current handoff for SWMMU/NOMMU development. Keep
release-facing feature descriptions in `Documentation/mm/swmmu.rst`, and keep
reproducible kselftest build instructions in
`Documentation/mm/swmmu-kselftest-build.rst`.

## Project status

- **Current phase:** the validated SWMMU/NOMMU series and complete signed
  userspace package set are integrated. Continue the remaining architecture
  and performance investigations as separate, reviewable changes.
- **Handoff and CI branch:** `ci/uml-nommu` (the default branch and the only
  branch on which this ledger is maintained).
- **Implementation branch:** `feature-swmmu-nommu`; do not maintain a second,
  independently edited AGENTS.md there.
- **Current profile:** experimental paged SWMMU on UML with `CONFIG_MMU=n`,
  `CONFIG_NOMMU_SWMMU=y`, and `CONFIG_UML_NOMMU_SAS=y`.
- **Correctness baseline:** SVM-style all-access instrumentation with raw
  pointers and runtime address translation.
- **Userspace checkpoint:** instrumented musl, BusyBox, OpenSSL, Python,
  coreutils, nginx, zlib and apk-tools package profiles have been built and
  published as an exact-source signed archive. Protected-branch run
  `39579186764` consumed that archive and completed 78 jobs successfully, with
  one expected image-publication skip and no failures. All full LTP rows, six
  KUnit profiles and direct SWMMU kselftest produced complete artifacts.
- **Report checkpoint:** issue #4 comment `6314635056` contains the corrected
  report. Its direct kselftest row is 38 pass, 0 fail and 8 skip, and completed
  signal tests no longer create false crash alerts. The Pages artifact was
  uploaded. Its deployment returned HTTP 404 before Pages was enabled. Pages
  is now configured with GitHub Actions as its source; the next report run must
  verify a successful deployment.

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
  OpenSSL and broader applications. Nginx has exploratory CI HTTP/TLS smoke
  and wrk matrix coverage; this is not yet broad application validation.
- [ ] Define the final mode ABI and expand beyond the current x86_64 SAS scope.
- [ ] Address concurrency and performance after functional validation is
  stable.

## Active TODOs

### CI failure follow-up (2026-10-23)

- [x] Match the upstream NOMMU kit by defaulting all `um-nommu*` LTP rows to
  `LTP_VIRT_OVERRIDE=uml` (`7c5dd4c8385a`). The setting identifies virtualization;
  LTP skips oversleep assertions, so raw latency remains a separate metric.
- [x] Use the requested matrix order consistently in report tables and plot
  legends, with native last. Shared definitions and a saved-report refresh
  workflow are pushed through `bed5ce382da5`. Refresh run `39860161385` updated
  Issue 4 and Pages successfully; published measurements remain unchanged.
- [x] Fix host fatal-signal cleanup in `swmmu-nommu-exp` commit `35016cac12d5`.
  The handler re-sends the signal using the existing reset/default setup and
  uses `_exit` as fallback, avoiding sleeping kernel exitcalls and libc exit
  handlers in signal context. Local SAS tests passed SIGTERM/SIGINT three
  times each with zpoline off/on (12/12), with no residual UML process. Normal
  guest poweroff and preserved SWMMU selftests passed in both modes
  (38 pass / 8 skip / 0 fail each). Broader MMU/SMP CI validation is pending.
- [ ] Restore safe general zpoline instruction rewriting. The RX-write safety
  fix `8eec4b37` leaves repeated syscalls on SIGSYS. Same-kernel paired getpid
  medians are 1465 ns raw versus 203 ns with a diagnostic pretranslated copy.
  Do not deploy per-program patched copies or re-enable the unsafe RX writer;
  writable backing/alias access, mapping semantics and kernel-context locking
  must be handled correctly.
- [ ] Revalidate the full NOMMU LTP matrix with `uml` classification after the
  selected kernel fixes. `mmap04` cross-backend partial MAP_FIXED support/skip,
  `setns02` child-side IPC TCONF propagation and Kirk monotonic duration
  accounting remain separate follow-ups. Nginx TLS passed a matched
  direct-window one-worker test; standard worker-matrix validation is pending.
- Full evidence and prioritized queue are in Obsidian Codex-Logs
  `2026-10-23_065100+0900-report-order-getpid` and
  `2026-10-23_065600+0900-fatal-signal-fix`.

### SAS/SWMMU validation update (2026-10-18)

- [x] Reproduce `add_key03` in isolation with `CONFIG_KEYS=y`, loop support,
  and the corrected SWMMU kernel. After removing alias activation from the
  scheduler return hook, Kirk recorded 1 pass, 0 fail, and 0 broken. Earlier
  stacks showed a waiter in `munmap()` and a second task blocked while
  activating aliases under the same `mmap_lock`. Treat this as a promising
  lock-inversion fix; repeat it and complete the full LTP run before closing
  the issue.
- [x] Rerun the full loop-enabled SAS syscalls suite with this kernel. The
  previous full attempt never produced a final result; `accept4_01` faulted,
  `access02` failed, and the `add_key03` cleanup stall prevented completion.
  Compare against the archived plain-NOMMU result only after obtaining a
  complete run with matching skips and kernel config.
- [x] Build a separate plain-SAS config with `MMU=n`, `NOMMU_SWMMU=n`,
  `UML_NOMMU_SAS=y`, and vector networking, then boot it with the Alpine
  derived rootfs and a musl static-PIE smoke init.
- [x] Define and locally validate an initial fork-independent LTP smoke subset
  for plain SAS. Both normal SAS seccomp and time-travel SAS seccomp completed
  the nine-test subset with 8 passes, 1 configuration skip
  (`gettimeofday01`), and no failures or broken tests.
- [x] The earlier plain-SAS time-travel row completed in GitHub Actions with
  build, boot, KUnit, JSON/HTML LTP report, and artifact upload. It was useful
  for proving that SAS and BASIC time travel can coexist, but is no longer the
  retained plain-SAS matrix profile.
- [x] Validate the replacement non-time-travel `um-nommu-plain-sas` row with
  the full syscall suite built with `NOMMU=1` and explicit `SWMMU=n`
  assertions. LTP's NOMMU compile guards and runtime `TCONF` results, rather
  than a CI run-pattern subset, handle tests unsupported without fork. Run
  `39561868757` completed all 1,434 records with an empty run pattern and
  uploaded JSON and HTML.
- [ ] `NOMMU_SWMMU` currently depends on `!MMU`; an `MMU=y` plus SWMMU build is
  not a supported matrix. If desired, test normal MMU fork/COW as a separate
  control and label it clearly; it cannot validate SWMMU behavior.
- [x] Add GDB steps for exact-rootfs ELF identity, `add-symbol-file`, ET_DYN
  section addresses, and separately loaded FDPIC interpreter/executable to
  `Documentation/mm/swmmu-kselftest-build.rst`.

### TODO: Reproduce intermittent bind04/SCTP modprobe panic (2026-10-22)

- [ ] The CI run `39628932961` panicked while running `bind04` on the SAS zpoline profile. The recorded stack is `call_usermodehelper_exec_async()` on `kworker/u4:1` -> `kernel_execve()` -> FDPIC interpreter load -> hostfs `open()` -> host glibc `__libc_open()` / `__pthread_enable_asynccancel()`, with `RAX=0`. The CI log does not establish the helper argv or prove that modprobe caused the panic.
- A local diagnostic kernel rebuilt with the CI Ubuntu 22.04 / GCC 11.4.0 toolchain and the exact vector build invocation (`CONFIG_UML_NET_VECTOR=y CFLAGS+="-DCONFIG_UML_NET_VECTOR"`) boots with `vec0`. Its embedded `.config` intentionally still reports `CONFIG_UML_NET_VECTOR` unset under static-link configuration. In the CI Alpine image, 20 repeated `bind04` records each passed 6 subtests and skipped SCTP with expected `EPROTONOSUPPORT`; no panic reproduced. Each run issued both `net-pf-2-proto-132-type-1` and `net-pf-2-proto-132` modprobe requests, and both opened `/lib/ld-musl-x86_64.so.1` from a no-mm worker. This proves the path is exercised locally, not that it caused the CI panic. The local kernel included temporary printk diagnostics, which may affect timing.
- Follow up by replaying bind04 with a no-instrumentation kernel built using the same CI flags and image. If it recurs, capture the usermodehelper argv, the interpreter open return path and worker registers under GDB. Compare the CI guest setup and local module/rootfs state; keep the issue open until the fault is reproduced or a concrete trigger is proven. Do not attribute unrelated `EOPNOTSUPP` output to this panic without a matching stack or trace.
- Full local config, logs, JSON and runner scripts are recorded in Obsidian: [bind04 CI vector-build reproduction](https://github.com/thehajime/obsidian/blob/main/nommu-swmmu/Codex-Logs/2026-10-22_083900%2B0900-bind04-ci-vector-build/result.md), Obsidian commit `bc968fb`.

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

### TODO: Recheck the x86-64 GOTPCRELX linker relaxation workaround

`scripts/swmmu-cc` passes `-Wl,--no-relax` to SWMMU guest links. With GNU ld 2.44, a CPython link changed a relaxable indirect call through a GOT slot into a direct call to `_Py_NoneStruct`, causing SIGSEGV; the same Python objects and runtime linked with `--no-relax` printed its success marker and exited 0 under both `zpoline=0` and `zpoline=1`. The conversion is in GNU ld and was also reproduced without the GCC plugin. The relevant conversion code is still present in the official Binutils 2.46.1 release and upstream master checked on 2026-10-13, so keep the workaround until an upstream change is identified and tested.

- Track the upstream Binutils fix and identify its first released version; do not infer that a newer version is fixed from its version number alone.
- Once fixed, rerun the same CPython link and guest test with normal relaxation and `--no-relax`, under both zpoline modes. Remove the wrapper workaround only if normal relaxation passes and produces correct code.
- The workaround disables linker relaxation for SWMMU links and may increase code size or affect link optimization; quantify this before removing or narrowing it.

### Deferred investigations: compiler stack probes and plain NOMMU SMP

- [x] Handed off to a separate task; investigation remains pending there.
  Investigate SWMMU coverage of GCC-generated stack-clash probes. A local
  reproduction with Alpine edge GCC 15.2.0 and the SWMMU plugin from kernel
  `86ee6de52c4a2cfe46f613cecda7a8888808cadd` showed that dynamic `alloca`
  probes first appear as `UNSPEC_PROBE_STACK` in RTL `expand`, while fixed
  frame probes first appear in `pro_and_epilogue`; generated assembly retains
  accesses such as `orq $0,(%rsp)`, outside the current GIMPLE instrumentation.
  The exact `coreutils/wc` compile command and its package-build logs remain
  uncollected. Reproduce that build with the exact GCC/plugin versions and save
  the command, pass dumps, and assembly. Compare a GIMPLE-side implementation
  with a target-aware late RTL/backend implementation, preserving stack pointer,
  guard-page fault, and unwind/CFA semantics; never rewrite `%rsp` accesses
  indiscriminately. Add fixed-size, dynamic-size, and multi-page regression
  tests and validate them in a SWMMU guest. Keep the existing
  `-fno-stack-clash-protection` workaround until guest validation passes.
  Track the coreutils 4 KiB buffer change separately as a NOMMU stack-capacity
  workaround.
- [x] Handed off to a separate task; investigation remains pending there.
  Investigate SMP support for **plain NOMMU UML only**. The feature branch
  currently selects `UML_SUBARCH_SUPPORTS_SMP` under `X86_CX8 && MMU`, while
  upstream UML `next` commit `aa3e6faf620b1091ae85b07c0106918d48fcce05`
  selects it under `X86_CX8`. The feature branch already contains the queued
  lock selects and `arch/x86/um/asm/spinlock.h`; the extra `MMU` condition is
  the relevant Kconfig difference found so far. A plain-NOMMU
  `olddefconfig` with `CONFIG_MMU=n` and `X86_CX8=y` did not retain
  `CONFIG_SMP`, but no kernel source change, SMP build, or boot test has been
  made. Compare the target kernel revision and upstream SMP dependencies,
  determine whether a separate Kconfig change is sufficient, then build and
  boot with multiple CPUs and verify online CPUs, scheduler/worker behavior,
  and a same-condition single-versus-multiple-CPU workload. Do not infer
  SAS/SWMMU SMP support from a plain-NOMMU result; assess those modes separately
  and state the required runtime and kernel support.

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
- [ ] Complete the remaining SWMMU package closure, then extend nginx testing
  beyond the current CI smoke/benchmark matrix. OpenSSL validation has not
  started.
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

Previous protected-branch integration gate (2026-10-21): run `39579186764`
completed 78 jobs successfully, with one expected skipped image-publication
job and no failures. All 11 LTP targets produced complete JSON/HTML reports;
all six KUnit targets recorded 806 pass, 42 skip and 0 fail; direct SWMMU
kselftest recorded 38 pass, 8 profile skips and 0 fail. The RISC-V NOMMU row
completed 1,433 records after the unsupported pthread-based `futex_wait03`
case was skipped only for that profile. The exact-source signed release also
contains `zlib-nommu` and `apk-tools-nommu`; Alpine verified all 11 APKs and
both indexes. Issue #4 publication is complete. Pages was disabled during this
run and returned 404; the repository setting has since been changed to use
GitHub Actions. The report gate needs a new run to publish and verify the site.

### CI/report follow-up requested (2026-10-21)

- The LTP report retains a distinct `um-nommu-plain-sas` row and explains that
  it uses plain NOMMU SAS with seccomp and SWMMU disabled. SWMMU targets display
  as `um-nommu (sas-seccomp)` and `um-nommu (sas-zpoline)`.
- RISC-V NOMMU LTP build and test jobs remain implemented but are disabled by
  default. `run_riscv_ltp=true` on manual dispatch enables them; disabled runs
  label the report row explicitly.
- Netperf chart data now stores one payload per row with separate TCP_STREAM
  and TCP_MAERTS columns. Both directions for a profile use the same x value;
  STREAM is positive and MAERTS negative, with one legend item per profile.
- The benchmark matrix and all report charts now include the distinct
  `um-nommu-plain-sas` profile (SAS on, SWMMU off, seccomp on). The iperf3
  chart puts forward throughput above zero and reverse below zero at the same
  x coordinate per profile. The netperf chart uses absolute box widths so
  TCP_STREAM and TCP_MAERTS remain vertically paired on the same payload x.
- The nginx matrix also includes plain NOMMU SAS using the plain NOMMU nginx
  rootfs and kernel image. It is limited to one CPU; worker=4 is reported as
  unavailable because this profile has no `fork()` support.
- The previous nginx matrix excluded four CPUs for every NOMMU profile. Run
  `39579186764` succeeded for its full nginx matrix; the old worker-4 stall did
  not reproduce. Per user direction, the throughput-stall follow-up is closed
  for now. The separate historical worker SIGSEGV remains an independent item.
- Native lmbench printed `Pipe bandwidth: 3027.57 MB/sec` and then exited with
  137 after its 120-second timeout in run `39699280247`. Inspection of the
  exact compiler command showed the native host benchmark was still compiled
  with `CONFIG_NOMMU`, selecting lmbench's clone-based NOMMU pipe path. The
  CI now omits that define; verify `bw_pipe_64k` completes and the host
  benchmark job exits normally in the rerun.

### LTP candidate CI configuration follow-up (2026-10-22)

- [ ] Re-run the candidate LTP integration matrix after pinning the UML kernel
  test configuration in `.github/workflows/ltp-kernel.config`. The
  `hajime/nommu-runtime-upstream` LTP source ref no longer carries the
  downstream `tools/buildroot/ltp-add.config` file that the Linux CI workflow
  previously downloaded without checking HTTP status. That silently omitted
  `CONFIG_PID_NS`, `CONFIG_USER_NS`, and `CONFIG_FANOTIFY` in run
  `39699280247`: the plain SAS report then showed new `TBROK` results for
  `ioctl_ns01/04` (`/proc/self/ns/{pid,user}` returned `ENOENT`) and cleanup
  warnings in `fanotify10`. CI now owns the stable config and asserts key
  options after `olddefconfig`; verify the plain SAS results and classify any
  remaining differences against run `39561868757`.

- [x] Complete plain NOMMU CI validation. Runs `37711748667` and
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
- [ ] Keep the RISC-V Buildroot image on the previously proven
  `thehajime/linux:zpoline-nommu-v6.10` ref until the refreshed image and CI
  test pass. Then evaluate and pin a newer RISC-V NOMMU kernel independently
  from the UML `feature-swmmu-nommu` ref.
- [x] Handed off to a separate task; implementation and verification remain
  pending there. Replace the UML-specific `cpu_tasks[0]->pid` diagnostic in
  `mm/nommu_swmmu.c` with an architecture-independent current-task PID lookup
  and verify the generic code compiles for RISC-V.
- [x] Re-run RISC-V LTP build after fixing the install archive path. Run
  `37701275797` built and installed LTP successfully, then failed because the
  helper archived `/github/home/ltp-install` while the Buildroot container
  installed under `/root/ltp-install`. The archive now uses the actual install
  path; run `37707094706` was cancelled after its kernel ref checkout failed.
- [ ] Remove the temporary LTP skips for `ptrace06` on `um-mmu` and
  `perf_event_open03` on `um-nommu-seccomp` and `um-nommu-sas-seccomp` after
  their failure causes are understood and fixed.
- [x] Validate the three UML userspace profiles in CI: Alpine standard for
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
  that native/MMU images lacked a dedicated host-control BusyBox, that runtime `apk add`
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
  `37843548583` exposed false-positive LTP conclusions: the SAS profiles
  panicked while running guest Python, plain NOMMU panicked during Python exit,
  and NOMMU-seccomp's guest Python segfaulted. The host step hid failures from
  `docker exec | tee` without `pipefail` and did not require a valid report.
  Commit `7ba857697899` adds `pipefail` and a nonempty HTML report check; verify
  this guard in the next run.
- [x] Investigate NOMMU-SAS LTP startup under both seccomp and zpoline. Run
  `37843548583` confirms that Kirk's guest Python 3.12 starts, then both modes
  panic while Python is running, before the first LTP result is written.
  Determine whether Python and any native dependencies must be built with the
  SWMMU plugin; verify that hypothesis with a SWMMU-built Python package and
  record the exact failing component before treating either SAS LTP run as
  valid. Runs `38005074759` and `38006360842` confirm that the SWMMU-built
  `python3-nommu` package (Python 3.14.8) is installed in the SAS LTP image,
  but zpoline still panics before the first test result. Both logs repeat
  `addr=0x10027f40b0`, `ip=0x10004cd55d`, and nearly identical registers. The
  `arch/um/kernel/trap.c` message identifies a kernel-context fault
  (`is_user == 0`); a tentative runtime-offset mapping points into
  `netdev_wait_allrefs_any()` at `net/core/dev.c:11718`, but the mapped PC is
  inside an x86 instruction encoding, so validate the relocation/control flow
  before treating that source location as definitive. Preserve the raw logs
  under the corresponding Obsidian CI record. Run `39561868757` supersedes
  those failures: both modes completed 1,433 records and uploaded JSON/HTML.
- [x] Investigate abnormal completion in both non-SAS NOMMU LTP profiles from
  run `37843548583`. Plain NOMMU ran 1,433 tests (6,755 passed, 73 failed,
  4 broken, 1,039 skipped), then panicked while Kirk's Python exited through
  `exit_mmap()` / `vm_area_free()` (`Segfault with no mm`). NOMMU-seccomp ran
  tests through `fcntl30_64`, then guest Python segfaulted at address zero and
  produced no JSON/HTML report. Establish whether these failures share a kernel
  cause or need separate fixes; keep the `epoll_wait01` and `waitpid01` skips
  provisional until their underlying failures are understood. In run
  `39561868757`, both non-SAS NOMMU profiles completed 1,434 records, shut down
  normally and uploaded nonempty JSON/HTML.
- [x] Validate the LTP false-success guard in a new run. Run `37843548583` has
  overall conclusion `success`, but it used workflow SHA `47ddab30b50b`, before
  `7ba857697899` added `pipefail` and the nonempty HTML report requirement.
  Therefore its success conclusion does not mean the four NOMMU LTP jobs
  passed: SAS jobs produced no report, and NOMMU-seccomp uploaded a zero-byte
  HTML file. Confirm the new guard makes these conditions fail visibly.
- [x] Verify that benchmark plots are embedded in issue #4 after a run using
  commit `6758daaedbeb` or later. The previous run predates that change, which
  adds Markdown image embeds for chart files actually deployed to GitHub Pages.
  Keep the Pages report link and compare the displayed chart URLs with the
  deployed files. Issue comment `6311751119` embeds all six immutable chart
  URLs from run `39561868757`; protected-branch Pages deployment remains the
  final publication check.
- [x] Verify UML ccache restore and save. Run `37761713019` restored exact
  immutable keys such as `Linux-um-nommu-ccache-build`, so the cache action
  skipped its post-job save. Run `37767571411` then missed its old
  `Linux-um-mmu-tt-ccache-build` cache because the restore prefix had an extra
  trailing hyphen, but successfully saved
  `Linux-um-mmu-tt-ccache-build-37767571411-1`. The key includes `github.run_id`
  and `github.run_attempt`. The restore prefix was later changed to match the
  legacy key, but run `37998928481` showed that this selected stale entries over
  newer per-run caches. The prefix now ends in `-` to select run-specific keys.
  Run `37770276697` restored a previous cache and saved a new key for each of
  the four UML profiles. GitHub reports 22 active Actions caches occupying
  10.67 GB (9.94 GiB), near the default cache quota; monitor eviction.
- [x] Verify the corrected UML cache restore prefix with focused run
  `38003391527` (https://github.com/thehajime/linux-private/actions/runs/38003391527).
  It restored run-specific cache keys: um-mmu from `37998928481`, um-mmu-tt
  from `38002524303`, and um-nommu / um-nommu-sas from `37998928481`. All four
  build-0 steps completed under one minute: um-mmu 44s (99.77% hits),
  um-mmu-tt 17s (99.77%), um-nommu 32s (99.68%), and um-nommu-sas 35s
  (99.69%). The focused run did not run KUnit, LTP, iperf3/netperf, or guest
  benchmarks. Its initial attempt failed only because the temporary test
  workflow omitted `/usr/lib/ccache` from PATH; that harness error was fixed
  before the successful run.
- [ ] Verify BusyBox `cd -` in the generated NOMMU binary before publishing an
  image.
- [ ] Upstream the `busybox-nommu` and `musl-nommu` APKBUILD changes to Alpine.
- [ ] Keep ordinary CI image builds local. Publish the stable
  `3.20.3-um-nommu` image only after the UML matrix succeeds and by explicit
  dispatch; record kernel/aports revisions and the image digest.
- [ ] Propose the UML `.lbss` linker-script placement fix upstream after
  validating static and dynamic UML builds against the `uml/next` base.
- [x] Implement and compare the full SWMMU/non-SWMMU benchmark suite: lmbench,
  getpid, iperf3, netperf, and LTP.
- [x] Publish benchmark results in a durable, reviewable location. Investigate
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
planned package sequence remains coreutils first, followed by broader nginx
validation and then OpenSSL. nginx now has initial CI HTTP/TLS smoke and wrk
matrix coverage; one SAS/seccomp 4-worker/64-connection warmup stalled, so this
does not close broad application validation. OpenSSL has not been started.

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

#### Task B netperf-only CI comparison (2026-10-12)

- [x] Add a focused workflow that runs the same UML kernel image, hostfs
  rootfs, netserver, and 12 netperf cases in a Docker-entrypoint process. The
  corrected CI run `38210659301` passed all 12 Docker guest cases (TCP_STREAM
  and TCP_MAERTS, 64-byte messages, 1- and 10-second durations, 3 repetitions).
  The 10-second cases each reported about 10 seconds elapsed. The one-off
  diagnostic workflow and guest init were removed after the results were
  recorded; the main workflow owns ongoing profile-wide measurements.
- [ ] Investigate the direct-host run, which failed all 12 cases with netperf
  status 255. The guest showed a route via `172.17.0.1`, but the host-launched
  control could not ping that address and had no neighbor entry. Determine why
  the direct-host UML network path is unreachable, then make a fair comparison
  with the Docker case using the same guest-visible network topology and
  instrumentation. Do not attribute a speed difference to Docker until both
  paths reach the netserver and complete the same cases. Preserve failed-case
  status in the report; the first run `38210297986` incorrectly reported
  success from the UML process exit status and was corrected in `38210659301`.
  Keep the direct-host connectivity and performance comparison TODO open; use a
  future focused harness if that comparison is resumed.

#### Task B TCP buffer delay follow-up (2026-10-13)

- [x] Remove enlarged `tcp_wmem` and `tcp_rmem` values from netperf runs.
  Netperf-only runs leave guest defaults untouched; combined runs keep the
  existing enlarged values for iperf3 and restore the guest's boot-time values
  before starting netperf. `.github/workflows/netperf-bench.sh` logs the
  effective values. Local
  matched runs with kernel `86ee6de52c4a2cfe46f613cecda7a8888808cadd`, the
  cached SAS rootfs, and `seccomp=on` showed default-buffer 10s/1500B timings
  of 17s STREAM / 29s MAERTS; setting `tcp_wmem` alone took 64s / 27s, and
  setting `tcp_rmem` alone took 17s / 128s. With both settings, the 64-byte
  1s/10s cases took 60s/63s STREAM and 110s/138s MAERTS; defaults took
  9s/18s STREAM and 20s/29s MAERTS. The exact local matrix and logs are in
  the Obsidian record `2026-10-13_090657+0900-netperf-seccomp-buffer-isolation`.
  The matched comparison shows that these enlarged values cause direction-
  specific wall-time delays in this local setup. The mechanism behind the
  extra UML CPU time is still unknown and needs investigation.
- [x] Confirm the netperf-only Actions path leaves `tcp_wmem` and `tcp_rmem`
  at guest defaults. Run `38329251963` completed all 15 cases in both
  `um-nommu-sas-seccomp` and `um-nommu-sas-zpoline`; the reported per-case
  durations and wall times are now in its artifacts and Issue #4 report. This
  focused manual run did not exercise the combined iperf3-plus-netperf path.
- [ ] Diagnose why the enlarged `tcp_wmem` setting delays TCP_STREAM and the
  enlarged `tcp_rmem` setting delays TCP_MAERTS outside netperf's reported
  measurement interval. Record guest/UML CPU use, relevant syscall and vector
  network counters, and compare setup, transfer, and teardown phases. Confirm
  which work accounts for the 100+ second local cases before proposing a
  further buffer change.
- [ ] Evaluate whether a different UML network device or supported backend
  can avoid the vector/raw transport overhead. Inventory the drivers/backends
  available in the hosted runner and their required capabilities, then compare
  a reachable host `netserver` workload under the same kernel, rootfs, payload,
  duration, and buffer defaults. Adopt an alternative only if the same cases
  complete reliably and improve wall time; record unsupported runner features
  and any topology differences.

- [ ] Diagnose large-payload netperf failures seen in run `38333192770` after
  removing the enlarged TCP buffers. The 256-byte cases reported connection
  refused; later payload cases included a netperf `Assertion failed: width >= 1`
  and userspace faults. Keep these failures visible in CI reports and determine
  whether the control connection, guest netserver, or UML vector transport fails
  first before changing the buffer defaults again.

### CI plot and full-run follow-up (2026-10-13)

- [x] Render and visually review the requested palette update: keep the MMU
  colors consistent, set SAS-seccomp to blue and SAS-zpoline to royal blue,
  and reuse the previous SAS colors for plain NOMMU. Check that the lmbench
  bandwidth plot no longer has excess right padding and that the nginx legend
  sits above the bars without overlap. The six charts in run `39561868757`
  were inspected from the report artifact; the palette, bounds and legends
  satisfy these checks.
- [x] Re-run the complete manual CI with KUnit, LTP, native network benchmarks,
  all UML benchmark profiles, and the nginx matrix enabled. Measure end-to-end
  time and compare it with a full run from the RISC-V image-build period; do
  not compare a shortened manual run to the complete run.
- [x] Verify in the Actions web log that nginx UML console output streams while
  the guest runs, including its boot/readiness and HTTP/TLS/wrk milestones.
- [x] Diagnose run `38350772429`: all 36 nginx matrix legs failed before the
  guest or benchmark started. The workflow executed `run-nginx-benchmark.sh`
  directly even though it is stored as mode `100644`, producing exit 126
  (`Permission denied`). The step now invokes it through `bash`.
- [x] Repeat the full CI after this harness fix. The nginx measurements and
  plots from run `38350772429` are invalid; do not use them for comparison.

### CI, rootfs, and plot follow-up (2026-10-13)

- [x] Consolidate shared nginx rootfs generation into one job. It builds one
  SAS, one MMU, and one plain-NOMMU rootfs and uploads each once for matching
  matrix jobs; differing userspace package sets mean these remain three images.
  The published UML Dockerfile already starts from Alpine edge and uses `apk
  update`/`apk add`. The standalone nginx rootfs builders now also copy Alpine
  edge repositories and Alpine plus NOMMU package signing keys into each
  guest, so repositories and trust keys are present for later package installs.
  Runtime `apk add` execution inside the SWMMU guest has not yet been validated;
  the custom NOMMU APKs remain available as signed APK artifacts.
- [x] Keep shared rootfs job/artifact/helper names generic where the assets are
  shared. SWMMU-specific package build and init helper names remain specific to
  the SWMMU-only contents.
- [x] Configure all benchmark plots from one shared profile palette: SAS-seccomp
  uses blue, SAS-zpoline uses royal blue, and plain NOMMU reuses the previous
  SAS colors. Keep the bandwidth chart's x-range tight, use at least +/-50 Gbps
  for netperf (expanding symmetrically when measured values require it), and
  leave headroom above nginx bars for its in-chart legend. These plot changes
  need visual confirmation from the next full Actions run.
- [x] Make nginx UML console output stream into the Actions job log while the
  guest runs, with explicit boot, readiness, smoke-test, and wrk phase markers.
- [x] Run the complete manual CI with KUnit, LTP, native network benchmarks,
  UML benchmark profiles, and nginx matrix enabled. Compare its elapsed time to
  the full CI baseline from the RISC-V image-build period; shortened manual runs
  are not comparable. If that period has no complete run, record the closest
  available run and explain the gap.
- [ ] Diagnose large-payload netperf failures seen in run `38333192770` after
  removing enlarged TCP buffers. The 256-byte cases reported connection
  refused; later payload cases included `Assertion failed: width >= 1` and
  userspace faults. Keep failures visible in reports; determine whether the
  control connection, guest netserver, or UML vector transport fails first
  before changing buffer defaults.
- [x] Close the historical nginx throughput-stall follow-up for now, per user
  direction. It has not reproduced in the official protected-branch matrix
  `39579186764`, including SAS-seccomp, workers=4, connections=64. The prior
  delayed-response observation and suspected userspace overhead remain useful
  historical context, but no root cause was established. A separate historical
  worker SIGSEGV entry below remains distinct and unresolved.
- [ ] **Unresolved: reproduce and root-cause the intermittent SAS/SWMMU nginx
  worker SIGSEGV from run `38397978801`.** The CI mitigation that tolerates a
  failed wrk warmup and extends request deadlines is not a fix for this fault.
  Under `um-nommu-sas-seccomp`, workers=4, connections=64, the guest recorded
  `nginx[39]: segfault at 100002fae3 ip 000000100002fae3 sp 0000001000277eb0
  error 14`. The exact nginx ELF maps the faulting IP to `ngx_pcalloc+0x21`,
  immediately after `nommu_swmmu_memset_dynamic` returns. This locates the
  fault, but does not establish its cause. That run completed zero requests in
  the measured 30-second cohort; the 64 GET payloads were TCP-acknowledged but
  no HTTP 200 response was captured. Connections=4 also had no completed
  warmup requests, while connections=16 had very low throughput. A search of
  nginx CI failure logs from 2026-10-10 through 2026-10-14 found this as the
  only confirmed natural worker SIGSEGV; four later JST 2026-10-14 runs had no
  confirmed SIGSEGV across 12 SAS/seccomp worker=4 legs, though throughput
  stalls continued. The run's kernel source SHA was
  `ac5599c5de10c4180f8ebd85bffd04ac3a8ec274`. An exact-rootfs local
  Raw-transport attempt did not reproduce it; faults from other diagnostic
  images, injected signals, and client-side crashes are not equivalent
  reproductions.
  - Reproduce on a TAP-capable runner using the exact run kernel image, SAS
    rootfs/APKs, nginx worker=4 configuration, wrk 4.1.0 client, and relevant
    CPU/resource limits. Repeat connections=4/16/64 and include worker=1 and
    SAS-zpoline controls.
  - On the first natural fault, capture worker PID, signal, registers, and
    faulting instruction; resolve RIP and fault address against the exact nginx
    ELF. Preserve kernel console, nginx error log, wrk output, packet capture,
    and host UML thread/stack samples for every attempt.
  - Correlate per-second completed requests and worker lifetime with accept and
    listen queues, TCP GET/ACK/HTTP response timestamps, host UML CPU/WCHAN, and
    bounded SWMMU activation/map/unmap timing and counters.
  - Distinguish a natural worker fault under load from warmup-only zero
    throughput, timeout, HTTP client errors, startup faults, and deliberately
    injected SIGSEGVs. Report whether the fault address follows an SWMMU alias
    or lifetime transition, an nginx/runtime defect, or another cause.
  - Do not close this item after one passing run or a test using a different
    network transport. Record matching kernel/rootfs/config hashes and the
    repeated-run denominator before estimating fault frequency.
- [ ] Add a distinct plain-NOMMU UML + SAS profile with SWMMU disabled in a
  separate CI task. Define its kernel config, rootfs packages, and validation
  independently; do not merge it into the current SWMMU profile work.

### Task C — plain-NOMMU mremap Maple range candidate (2026-10-13)

- [ ] Keep the historical `virt_to_slab()==NULL` teardown panic open. The Maple range update is a root-cause candidate for a confirmed Maple invariant violation, but has not yet been shown to prevent the historical panic under its reproduced 20-iteration workload.
- On diagnostic source `86ee6de52c4a2cfe46f613cecda7a8888808cadd`, `do_mremap_nommu()` changed `vma->vm_end` without updating the Maple Tree range. A following shrink/unmap could leave a stale Maple entry after VMA teardown. `validate_mm()` probes exposed the inconsistency.
- Candidate source change is tracked in [PR #13](https://github.com/thehajime/linux-private/pull/13), targeting `feature-swmmu-nommu`. It updates the Maple range on shrink and grow.
- Focused probes for shrink then unmap, and shrink then re-expand then unmap, completed with no validator warnings. On the diagnostic image, a mixed `mmap|munmap|mremap|brk` LTP profile completed 2 iterations (38 pass, 8 fail, 0 broken, 46 skipped), with no panic and normal guest teardown. This is not proof against the historical reproducer, which required 20 iterations. A 20-iteration attempt was interrupted during the third `munmap04`, after two full iterations, so it provided no teardown result.
- Next: build the candidate on the vector-enabled test image and run the full 20-iteration workload through teardown, retaining `validate_mm()` diagnostics. Close only after the panic is checked against that workload; if it still occurs, continue root-cause investigation.
