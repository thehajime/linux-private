# SWMMU project handoff

This file is the concise, current handoff for SWMMU/NOMMU development. Keep
release-facing feature descriptions in `Documentation/mm/swmmu.rst`, and keep
reproducible kselftest build instructions in
`Documentation/mm/swmmu-kselftest-build.rst`.

## Current TODO index

This index is the authoritative list of open work. The detailed task records
and historical ledger below retain evidence and earlier decisions, but an old
unchecked box there does not make a task current unless it is listed here.

### Kernel

- **K1 — Default-off fork:** design general fork-time migration and separate
  user virtual addresses from SWMMU backing without partial-mm failure states.
- **K2 — Final modes and concurrency:** define the mode ABI, non-SAS path,
  SMP/multithread rules, and safe concurrent zpoline text rewriting.
- **K3 — Remaining correctness:** close user-copy, mapping, signal, nonlocal
  control-flow, and compiler-generated access gaps.
- **K4 — Abrupt UML cleanup:** restore safe child/device cleanup after
  `last_ditch_exit()` without sleeping in a signal handler.
- **K5 — Intermittent bind04 helper fault:** retain as unreproduced until a
  matching usermodehelper fault is captured or ruled out.
- **K6 — GOTPCRELX workaround:** track and validate an upstream binutils fix
  before removing `--no-relax`.

### Tests

- **T1 — LTP upstream series:** finish glibc regression validation and hand the
  signed 12-patch series to the upstream-submission task.
- **T2 — Expanded nginx matrix:** validate the profile-grouped 1/4/8/16 worker,
  connection, and supported CPU measurements, then refresh issue #4.
- **T3 — Kirk upstream series:** submit the preserved runtime fixes from
  `hajime/kirk-runtime-fixes-review` in the separate Kirk task.
- **T4 — Future LTP gate:** keep exact selected-test coverage, normal shutdown,
  and zero FAIL/BROK/WARN as required CI acceptance conditions.

### Compiler plugin

- **P1 — Generated accesses:** cover stack-clash probes, late RTL/backend
  accesses, spills/reloads, atomics, vector accesses, and inline assembly.
- **P2 — Final access policy:** define the production balance between
  all-access correctness fallback and proven native-access optimization.

### Userspace

- **U1 — Plain NOMMU nginx workers:** validate the independent `SO_REUSEPORT`
  instance implementation in CI and retain its observed-process-count gate.
- **U2 — Alpine upstreaming:** prepare the remaining musl/BusyBox/apk profile
  changes for their appropriate upstreams after kernel/runtime interfaces settle.

### CI, reports, and documentation

- **C1 — Nginx acceptance:** run the expanded per-profile jobs, preserve every
  measurement, and update plots/report only from a complete run.
- **C2 — TODO maintenance:** update this index and its matching detail whenever
  a task is added, completed, superseded, or moved to another repository.

## Current TODO details

### Kernel details

**K1/K2:** The current accepted configuration is default-on paged SWMMU with
SAS. General default-off fork migration, user-VA/backing separation, non-SAS
alias synchronization, and general SMP-safe rewriting remain design work. Keep
failure handling transactional and preserve parent/child private COW semantics.

**K3:** The remaining coverage includes aggregate and bitfield user-copy,
file-backed/shared/device mappings, partial protection changes, signal frame and
restart behavior, setjmp/longjmp, compiler-generated accesses, and transactional
remap rollback. Do not infer these from the completed syscall LTP matrix.

**K4:** Normal halt/reboot runs cleanup. Host SIGINT/SIGTERM now avoids unsafe
cleanup in signal context, but a safe deferred shutdown path for ptraced children
and devices is still required.

**K5/K6:** The bind04 signature was exercised repeatedly without reproduction;
keep its exact historical stack and build conditions in the ledger. The CPython
GOTPCRELX failure remains avoided with `--no-relax`; retest both link modes only
after a concrete binutils fix exists.

### Test details

**T1:** LTP branch `hajime/ltp-mmu-native-fixes-upstream` contains 13 reviewable
patches at `a751a3a48`. Every patch carries `Signed-off-by: Hajime Tazaki
<thehajime@gmail.com>` and its subject is free of CI-control tags. Ubuntu 24.04
glibc 2.39 built the changed library and syscall directories. Focused execution
passed sendmsg01, both signal-wait tests, prctl10, sbrk01, readlink03,
readlinkat02, setgroups03 and recvmmsg01; kcmp03, the three core-dump tests and
reboot01 produced configuration skips under the container kernel. seccomp01
passed four strict-mode checks but its filter-mode setup needs an environment
with initial-user-namespace `CAP_SYS_ADMIN`; the unprivileged container returned
EACCES. The loop-device and memory-headroom library changes compiled but need
their privileged/full-suite upstream CI coverage. Preserve these limits in the
upstream handoff instead of calling the glibc run fully complete.
The thirteenth patch sizes legacy `sigaltstack01` from `AT_MINSIGSTKSZ` when a
large host XSAVE frame exceeds compile-time `SIGSTKSZ`. It addresses the native
ENOMEM seen in CI `40078799660`; its upstream-branch distro build matrix
`40083112594` passed 16/16 jobs. Runtime CI must use `659c79cba`, which is the
same signed patch alone on accepted NOMMU runtime base `1bb350a1`; the upstream
series SHA lacks those downstream NOMMU framework commits.

**T2/C1:** The nginx workflow uses one GitHub matrix job per runtime profile.
Every profile measures workers and connections 1/4/8/16. MMU-seccomp and plain
NOMMU-seccomp also measure 1/4/8/16 online CPUs; PTRACE and SAS profiles remain
at one CPU. Missing measurements are failures, not inferred skips. Forkless
plain NOMMU implements worker counts with independent single-process nginx
instances sharing the listeners through `SO_REUSEPORT`; the harness rejects a
result unless the guest-observed nginx process count equals the request. CI
run `40075679419` was dispatched from CI commit `255b82664280` with kernel
`be7f38751f894520101c66c08748aba36b1584a4`, pinned aports
`a3816eafb3eeb8e1e673d9f43cde169bd684c5a1`, and LTP `fix-nommu`. It must
complete every expected nginx measurement before issue #4 is accepted as
refreshed. Its first attempt stopped at APK acquisition because the
timestamp-corrected feature commit has rewritten ancestry. Its tree
`a2a94cb570d274e63908a0066f2843af8d0e36af` is byte-identical to accepted
commit `6c0c5d71b58a`; the CI reuse gate now recognizes identical Git trees in
addition to its reviewed file-set rules. Rerun from the corrected CI commit.
Run `40075835158` passed that APK gate but exposed a moving-input problem:
`fix-nommu` had advanced from the accepted `1bb350a1` to `659b6f7f`, causing
the compatibility patches to be applied twice. It was cancelled before the
long matrix. Replacement run `40075930328` pins the accepted full LTP SHA
`1bb350a1cd55501173e33f1836a674494f2cb48b`; keep this and the kernel/aports
inputs immutable while assessing T2.
That replacement exposed an nginx matrix parameter error before UML nginx
started: wrk rejects `connections=1` with two client threads. Use one wrk
thread for the one-connection cell and two threads for connections 4/8/16 in
both native and UML profiles. Full immutable-input rerun `40076692042` uses CI
commit `17fe40492ffd`; accept T2 only after all profile artifacts and the
aggregate report are complete.
That run's first plain NOMMU cell reached the generated nginx config and then
exited init with status 1. The multi-instance polling loop used false `test &&`
lists under BusyBox `ash` with `set -e`; rewrite those predicates as explicit
`if` statements so an ordinary non-nginx `/proc` entry or an asynchronous
startup wait cannot terminate PID 1. Retest every plain NOMMU profile.
The same run also exposed two harness defects. The profile driver passed
`--needs-rawio 1`, while the cell runner enables `CAP_SYS_RAWIO` and lowers
`vm.mmap_min_addr` only for the literal value `true`; zpoline therefore panicked
in `setup_zpoline_trampoline()` before userspace. Pass a boolean consistently.
In addition, remove `tap100` during unconditional cell cleanup so one guest
panic cannot make every later matrix cell fail with `could not create tap100`.
Run `40076692042` was cancelled before report publication after its diagnostic
artifacts confirmed both problems. Validate their fixes together with the init
fix in a replacement immutable-input run.
Replacement run `40078799660` confirmed that zpoline now applies
`vm.mmap_min_addr=0` and initializes its trampoline, and that TAP cleanup keeps
later cells independent. Plain NOMMU PID 1 still exits with status 1 after the
generated config passes `nginx -t`, so the original two polling predicates were
not the only `set -e` trigger. Keep command tracing and an exit-status marker in
this small benchmark init until the exact failing command is fixed; use the
run's immutable artifacts for focused profile replay instead of rebuilding.
Focused replay `40080246613` traced the remaining trigger exactly: BusyBox
`ash` treated the startup loop's final false condition (`[ 2 -le 1 ]`) as the
status of the whole `while` command and exited under `set -e`. Express both
bounded loops as `while :` plus explicit successful `break` conditions. Retain
the compact exit-status marker, remove verbose tracing, and replay the same
artifacts before another full run.
The reusable/manual nginx workflow has an opt-in `single_cell` diagnostic input
that restricts a replay to CPUs=1, workers=1 and connections=1. Keep the default
full matrix unchanged; use this input with immutable source artifacts for quick
boot/load/shutdown iterations before consuming runners on another complete run.
Single-cell replay `40082577298` passed HTTP and TLS smoke, warmup, all five wrk
samples, observed-worker validation and TAP capture at 1,624.69 requests/s with
zero socket errors. This validates the corrected plain NOMMU init for the
1/1/1 cell; the complete 16-cell replay remains the profile acceptance gate.
Do not use upstream-series SHA `a751a3a48` as the CI LTP runtime. Cancelled run
`40084497522` showed that its unrelated newer upstream base makes NOMMU test
binaries return status 2 and eventually panic in fcntl18. This was a ref/base
selection error, not evidence against the sigaltstack size fix. Use exact
runtime SHA `659c79cba8ec427eab701a9146ee6bfa1b08328f` for the replacement.

**T3:** The Kirk work remains separate from the LTP syscall fixes. Preserve its
monotonic timing, complete-output, EOF, cancellation, and no-suite-timeout
behavior as an independently reviewable series.

**T4:** `check-ltp-result.py` compares the JSON records with the exact runtest,
skip, and run-pattern selection. CI must fail for a missing/duplicate/unexpected
record, a non-PASS/TCONF/SKIP status, any FAIL/BROK/WARN assertion, inconsistent
totals, abnormal runner completion, or abnormal UML shutdown.

### Compiler plugin and userspace details

**P1/P2:** Keep the all-access path as the correctness baseline while measuring
later-pass transformation and kernel-backed native eligibility. Source-specific
allowlists are experimental evidence, not a production deployment interface.

**U1/U2:** Plain NOMMU uses one `master_process off` nginx instance per requested
worker, unique pid files, and `SO_REUSEPORT` listeners. Record guest processes
and fail mismatches rather than trusting the requested count. Keep package
source, compiler/plugin version, kernel SHA, and signed repository manifest
exact for every userspace comparison.

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
- [x] Reproduce fresh musl static-PIE kselftests in both zpoline modes.
  Latest local functional source 846ce0178c93 passes SWMMU 38/0/8,
  register/signal-state regression 15/0 and shared-file regression 3/0;
  complete console logs and exit codes are pushed in Obsidian.
- [ ] Close the remaining correctness gaps for signals, mappings, user-copy,
  nonlocal control flow, and supported compiler-generated accesses.
- [x] Extend the signed instrumented Alpine package profile through coreutils,
  OpenSSL, Python, nginx and apk-tools. Broader application acceptance remains
  separate: nginx has CI HTTP/TLS/wrk coverage with intermittent stalls still
  under investigation; package availability is not a complete correctness claim.
- [ ] Define the final mode ABI and expand beyond the current x86_64 SAS scope.
- [ ] Address concurrency and performance after functional validation is
  stable.

## Historical task ledger

The entries below preserve prior task state and diagnostic evidence. They are
not the current TODO index; use the categorized index above for scheduling.

### CI failure follow-up (2026-10-23)

Current remediation checkpoint (supersedes the original failure inventory
below; local and full CI acceptance are complete):

- [x] Native full syscalls on freshly built LTP `1bb350a1cd55501173e33f1836a674494f2cb48b`:
  1,481 records, 9,895 pass / 405 skip / 0 FAIL/BROK/WARN, normal exit 0,
  with exact selected-name coverage validated. Branch: `hajime/ltp-mmu-native-fixes`.
- [x] MMU seccomp full baseline on kernel `42f11eb7159a`: 1,481 records,
  10,046 pass / 514 skip / 0 FAIL/BROK/WARN, normal poweroff. A newer candidate
  still needs the same full acceptance after common timer/ptrace changes.
- [x] Diagnose uninitialized timer is_user with live GDB. Kernel `67ff6a9a07a7`
  initializes the timer frame and tracks separate-process user execution per
  task; SAS uses its existing PC classifier. `c2c5790d8558` also returns EFAULT
  for out-of-range ptrace register buffers. Focused TT tests pass 36 timing,
  6 clock and 21 ABI checks. Later branch commits are documentation-only.
- [x] Current SAS regression in both zpoline modes: parsed KUnit 34/0/1,
  SWMMU 38/0/8, FP/signal 15/0 and shared-file 3/0, syscall02 and mmap02 pass,
  with normal poweroff. This is not a freshly rebuilt full SAS LTP suite.
- [x] Combined-candidate TT and ptrace full syscalls: 1,481 records each,
  zero FAIL/BROK/WARN, exact coverage and normal poweroff. TT: 10,053 pass /
  513 skip; ptrace: 10,054 pass / 513 skip. Ptrace uses a TT-capable kernel
  with time travel disabled. Deliberate fault assertions audited separately.
- [x] Current normal CI SMP-config MMU seccomp full run: 1,481 records,
  10,046 pass / 513 skip / zero FAIL/BROK/WARN, normal poweroff and exact
  coverage. All 22 fault events match intentional tests. Prior msgstress01 PASS labels
  already included one runtime-budget TCONF; do not claim all stress pairs ran.
- [x] Port runtime prerequisites to CI: preserved extraction modes, native
  child reaping and memory budget, cgroup delegation, private loop hotplug,
  ext4 scratch, truthful UML override and explicit completion gates. Actual
  helper smoke tests pass native and UML; injected UML setup failure powers
  down without a false success marker. Full CI remains unrun.
- [x] Review exact APK compatibility: 52a8dfe6b58a -> 6c0c5d71b58a is now
  explicitly allowed, in addition to the older reviewed pair. Added changes
  are kernel uaccess/ptrace/MAP_32BIT/timer fixes and docs; no plugin/runtime/
  exported UAPI change. Source and signed manifest checks remain mandatory.

CI 39949026306 was cancelled before LTP execution after its build log exposed
3 GiB of host swap while the newly added native container limit prohibited
swap. ee7e73b9abb5 retains the resident-memory budget and allows host swap;
replacement run 39951978045 uses kernel 6c0c5d71b58a and LTP 1bb350a1cd55.
Actual native and MMU CI entrypoint/report smoke paths both pass 102 assertions
with one architecture skip and normal completion. This executor has no swap,
so actual swap I/O remains a CI validation item. Do not merge while CI is active.


CI 39951978045 native completed but is not accepted: epoll_pwait03 has three
oversleep failures because Alpine could not detect the Azure VM. setsockopt11
newly tainted the runner kernel; wait403/io_uring04 then BROK on taint D.
The exact host fault trace is missing. Detect runner virtualization before
Docker creation and preserve native before/after dmesg/taint snapshots. Do not
skip these tests or merge until the native kernel failure is understood.
Local epoll_pwait03 with the actual host VM override passes 14 assertions.

Diagnostic run 40020480233 isolated the failure on the same
6.17.0-1022-azure runner kernel. epoll_pwait03 passed 14 assertions, wait403
passed, and io_uring04 configuration-skipped because RDS is unavailable.
setsockopt11 changed kernel taint from 0 to 128 and captured a NULL dereference
in skb_release_data() through __ip6_flush_pending_frames(). This is the bug
fixed by 736b380e28d0, which that hosted kernel lacks. Native CI excludes only
setsockopt11 on the exact diagnosed kernel release; an updated runner
automatically runs it again. Preserve this as a configuration exclusion, not a
passing assertion. The main full CI still needs to verify the complete result.

Full JSON, HTML, configs, GDB evidence and exact commands are in Obsidian
`nommu-swmmu/Codex-Logs/2026-10-23_175737+0900-ltp-local-abi-and-runtime-fixes`.
Corrected full CI 40020987840 completed 99/99 jobs successfully on CI
eebc9ead90e1, kernel 6c0c5d71b58a and LTP 1bb350a1cd55. All 11 full LTP
profiles pass exact selection coverage and normal termination: 16,253 records,
87,866 pass / 9,394 skip / zero FAIL/BROK/WARN. Native taint remains zero
before and after. Six KUnit profiles total 4,836 pass / 252 skip / zero errors;
both direct SWMMU modes total 112 pass / 16 skip / zero failures. The result
report is issue #4 comment 6375108650. Validated head 6c0c5d71b58a was
fast-forwarded to feature-swmmu-nommu, review PR #17 was closed, and the
redundant swmmu-nommu-exp branch was deleted.

Native bind06/setsockopt06 can temporarily set max_user_namespaces=1024 and
prevent cloud sandbox creation with ENOSPC. Confirm test cleanup restores the
saved value; do not confuse this with disk exhaustion or reset the environment.


- [ ] Keep general default-off fork migration, user-VA/backing redesign and
  general multithread/SMP-safe instruction rewriting as separate architecture TODOs.
- [x] Full CI 40020987840 validated the exact candidate; swmmu-nommu-exp was
  fast-forwarded into feature-swmmu-nommu at 6c0c5d71b58a, PR #17 was closed,
  and the redundant exp branch was deleted.
- [ ] Kirk upstream preparation is preserved in thehajime/ltp branch
  hajime/kirk-runtime-fixes-review, tools/kirk/runtime-fixes. This is a staged
  patch series, not an LTP submodule update or a submitted Kirk PR.

- Review follow-up: implementation is now integrated into
  feature-swmmu-nommu. New trap organization moves SAS PC
  classification and invalid-zpoline recovery into the formerly unbuilt
  arch/um/nommu/trap.c; common signal delivery remains in kernel/trap.c.
- IMPORTANT: last_ditch_exit handles host SIGINT/SIGTERM. Commit 35016cac12d5
  avoids unsafe signal-context cleanup, but also removes that path's exitcalls
  and ptraced-process cleanup. Normal guest halt/reboot still cleans up.
  Audit abrupt-exit child/device cleanup and a safe out-of-handler shutdown
  mechanism before considering this review item closed; do not restore sleeping
  cleanup inside the signal handler.
- nginx charts now use CPUs on x, profiles together and connection-count panels.
  UML wrk measurements repeat five times per boot for real boxplot samples;
  historical single samples and native single runs remain explicit diamonds.
  These are within-boot distributions, not confidence intervals across boots.
- Full local syscalls runs started for native and MMU ptrace/seccomp with the
  five LTP candidates, normal native port boundary, shadow and prepared cgroups.
  Kirk in this artifact rejects -1; a local one-line scheduler patch makes 0
  mean no suite timeout. Keep this separate from existing monotonic/EOF fixes.

- User reopened the existing MMU/native baseline failures as an explicit goal:
  reduce FAIL/BROK/WARN to zero, without disguising unsupported tests as passes.
  The earlier acceptance decision to exclude baseline failures does not close
  this new task. Full-suite zero-error acceptance remains pending.
- Local first batch: readlink03/readlinkat02 test raw syscall validation;
  setgroups03 uses the kernel ngroups_max; seccomp01 permits libc fork(2);
  recvmmsg01 isolates both deliberately invalid pointer cases. These five
  tests pass 45 assertions, zero fail/broken/warnings, on native and the exact
  CI MMU kernel in both ptrace and seccomp modes. The libc-invalid-pointer
  child SIGSEGV is intentional; raw syscall variants still require EFAULT.
  Patch is carried in .github/workflows/patches/ltp-musl-syscall-errors.patch;
  the external LTP upstream-review branch has not been rewritten.
- Native bind02 passes with ip_unprivileged_port_start=1024; prot_hsymlinks
  passes with shadow installed. CI container/rootfs prerequisites are updated.
- Remaining: signal wait tests loop because musl retries EINTR; sendmsg01
  invalid cmsg pointer; unsupported musl sbrk growth; MMU MAP_32BIT, ptrace,
  PR_SET_TSC, request_key; kcmp03 timeout (CHECKPOINT_RESTORE absent); native
  cgroup delegation, filesystem/device/reboot namespace assumptions, timing
  and taint-dependent cases. Diagnose before selecting fixes or TCONF policy.
- Cgroup diagnosis: enabling memory in the populated container cgroup root
  returns EBUSY. A dedicated container moving its init into a runner leaf
  before enabling memory passes mmap22; three other cases stop at truthful
  no-swap TCONF. Production CI cgroup lifecycle handling is not yet changed.
- Issue #4 cleanup backed up all 22 old comments in Obsidian before deleting
  19 obsolete/failed reports. Retained three successful default-branch reports
  now fold KUnit details; generator updated. nginx CPU/worker scaling chart
  proposal is saved with raw measurements; no measurement values were changed.

- [x] Restore safe plain SAS zpoline text rewriting in work-branch commit
  `ee7a1b92d18a741efee6a30eb68ce7b7ec0c4ced`. Only exclusively owned executable
  VM_MAPPED_COPY regions are eligible; a temporary RW/non-executable host alias
  updates physical backing under mm/region locks and preemption exclusion.
  Shared/direct regions, SMP, cross-page instructions and invalid syscall
  numbers retain fallback. Matched local getpid medians: rewrite-disabled
  plain SAS(z) 1406.526 ns, candidate seccomp 1386.313 ns, candidate zpoline
  264.931 ns (three million-call samples each). New plain test passes 9/9 in
  both modes with normal shutdown. Both SWMMU modes pass KUnit 34/0/1,
  SWMMU selftest 38/0/8, FP/signal 15/0, shared-file 3/0 and syscall02/mmap02.
- [ ] Run full plain-SAS CI acceptance for `ee7a1b92d18a` before integration.
  The previous successful full CI below does not include this new fix. Stage
  the new nommu_zpoline_native_test with a hostfs private-copy filename and a
  ramfs shared filename in both plain modes; any kernel/APK reuse allowlist
  update must review the exact new delta. No userspace runtime/UAPI changed.

- Final acceptance: CI 39888448097 completed success (97 successful jobs,
  2 intended skips). All seven NOMMU LTP profiles have zero fail/broken;
  MMU/native affected tests all occur in both saved pre-fix baselines.
  Generic KUnit totals: 4836 pass, 252 skip, zero fail/crash/error; dedicated
  SWMMU KUnit 34/0/1 is separately local. Corrected selftests: 110 pass,
  18 skip, zero fail across both modes (AVX-512 absent on the CI CPU).
  Issue 4 comment 6356910713 and Pages were refreshed successfully in
  39897200876, including canonical mode labels and original TAP reaggregation.
  Current CI startup to accept01: SWMMU zpoline 5.884 s, seccomp 9.519 s,
  plain SAS seccomp 0.699 s. These are host-timestamp CI observations,
  not a controlled cold-pyc rerun. Work-branch kernel 8274a8c5b996 remains
  unmerged; CI tested functional source 846ce0178c93cb91d1aeb6dff2ad4c71a6d15b00.

- Historical completion audit: musl bytewise/chunk/direct-bulk comparison was
  completed on 2026-10-20 (18 matched boots; help medians 4.47/2.62/2.40 s,
  first-test medians 8.08/4.20/3.95 s). Python general guarded-access work also
  completed then (first-test median 3.45 s, help 1.95 s). These historical
  measurements must not be mislabeled as open work or as today's timings.
  Default-off arbitrary fork remains a held, fail-closed design limitation,
  separate from the current default-on acceptance path.

- [x] Fix reproducible guest SSE context crossover in `a637af743264`.
  Parent/child sched_yield probes on the exact c0 CI kernel observe the other
  process's XMM15 pattern in both zpoline modes. Saving before alias activation
  and restoring immediately before user return eliminates the standalone
  failures. The new nine-case selftest fails only its added FP test on c0,
  and passes three fresh boots per mode on the candidate; KUnit 34/0/1,
  SWMMU selftest 38/0/8 and syscall02 also pass, with normal poweroff.
- [x] Fix AVX/AVX-512 preservation in `a75dc74fb20e`, using enabled user
  XSAVE components fitting the host image and aligned per-task storage.
  `846ce0178c93` also validates signal-return MXCSR/XSTATE before installing
  the image. Both modes pass the extended 15-case register/malformed-frame
  test, KUnit 34/0/1, SWMMU 38/0/8 and syscall02, with normal poweroff.
- [x] Fix the newly exposed mmap02 NULL-file panic in `27f78527df26`.
  Anonymous shared LTP counters expose a fileless region in the shared-file
  reuse scanner. A minimal tmpfs-backed probe reproduces c0's exact fault;
  six candidate boots pass the new 3-case regression and actual mmap02.
- [x] Complete final CI on the combined FP/mmap02 fixes (39888448097 success). The selftest job now
  stages all available regressions, runs both zpoline modes, retains separate
  TAP/exit files and requires normal guest poweroff. Kernel-only APK reuse
  accepts the reviewed 19-file delta; compiler/runtime/UAPI changes remain
  rejected. Three post-XSTATE local nginx repeats pass HTTP/TLS and 64-client
  wrk, but c0 also passed local repeats, so CI stall causation remains open.
- [x] Batch consecutive PFN aliases in `2dcda6ba8906`, preserving backing-file
  boundaries, per-VMA permissions and full-range failure rollback. Both modes
  pass KUnit/selftests, including partial-batch failure and retry. A same-binary
  diagnostic reduces traced mmap calls from 90,711 to 68,703, but does not
  establish an nginx throughput improvement or resolve the CI stalls.
- [x] Diagnose missing plain NOMMU lmbench numbers: benchmp was built for
  private fork, while only lat_proc selected vfork. A matching musl static-PIE
  control on the plain-SAS CI kernel reproduces empty output/exit 0 before and
  a numerical result with CONFIG_NOMMU's CLONE_VM harness. `6eda6139c0bc`
  adds that define only to nommu-plain builds. Completed run 39881692721
  confirms numerical results in all five affected profiles, with no FAIL cells.
- [x] Collect full corrected zpoline LTP results from 39878485571 and
  39881692721: each has 1,477 records (890 pass / 587 conf / 0 fail / 0 broken),
  6,714 passing assertions, 1,089 skipped assertions. clock_gettime04 passes
  6/0/0 in both; mmap04 is configuration-skip and setns02 passes. Raw JSON and
  generated HTML are pushed in Obsidian. Final combined-source CI 39888448097
  pins 846ce0178c93cb91d1aeb6dff2ad4c71a6d15b00; newer work-branch commits
  only normalize test formatting and document the new architecture behavior.
- [x] Collect corrected LTP/Kirk run `39878485571` (completed failure on c0)
  and batching/lmbench run `39881692721` (2dc kernel). Both hit the same
  mmap02 panic in SWMMU seccomp; neither includes the FP or mmap02 fixes. Run `39880693528` was canceled for short-
  SHA checkout failure; `d44397b6e727` now uses the package-preparation job's
  resolved full kernel SHA for checkout. A moving source branch cannot race
  that resolution. Do not classify that checkout failure as a kernel defect.
- The experiment deferring the first userspace alias activation was tested but
  did not establish a performance gain; its patch/logs are preserved in
  Obsidian, and the source experiment was reverted before the SSE fix.

- [x] Apply the candidate LTP shared-counter/configuration patch and Kirk
  complete-output/cancellation fixes through `78c75250643c`. Local four-test
  results are 8 pass / 3 skip / 0 fail / 0 broken in both zpoline modes, with
  native and injected-error controls. Keep clone06/clone08 enabled only for
  the modern SWMMU runtime framework with shared counters verified.
- [x] Validate those fixes in full CI 39888448097. Older run `39866498606` completed
  both SWMMU LTP rows with 1,475 records each: seccomp 887 pass, 585 conf,
  1 fail, 2 brok; zpoline 886 pass, 586 conf, 1 fail, 2 brok. The affected
  records are clock_gettime04, mmap04 and setns02. Its old Kirk channel loses
  buffered output and its old private IPC loses child counters, so assertion
  aggregates are not final acceptance data. Do not silently compare them with
  counts produced by the corrected framework.
- [x] Revalidate intermittent nginx SWMMU zpoline startup/64-client stalls.
  Final CI passes the full nginx matrix, followed by three passing immutable
  artifact 4-worker/64-client replays (39894238540/39894241245/39894244213).
  Historical failed-replay evidence below remains; a single causal explanation
  is not inferred from the newer throughput results alone.
  Exact source assets from `39866498606` reproduce zero measured requests in
  five of six 64-client manual replays, with one passing replay. Four
  16-client manual replays pass. Three independent local boots using the
  exact CI kernel/rootfs pass 64-client measurements (2,128--4,502 req/s),
  plus an initial 30-second local run at 4,748 req/s. These differ in client
  build and host conditions and do not close the CI failure. GDB on a failed
  run catches multiple nginx tasks mapping aliases and later reaches idle;
  remapping is observed, but not proven to be the complete cause.
- [x] Replace SWMMU nginx's ineffective hostfs FIFO command bridge with a
  host PTY relay (`21c20f64b4dc`), then add repeated GDB task snapshots in
  `9dc1d5cd4265`. A hostfs FIFO has separate pipe state in each kernel.
  Six small local serial probes and passing real nginx replays terminate
  normally. Serial shutdown failed during one stalled workload; do not
  claim universal lifecycle correctness yet. Non-SWMMU control is unchanged.
- [ ] Investigate clock_gettime04 CI latency separately from correctness.
  In a controlled 0.25-CPU container, adjacent readings have 74--78 ms gaps;
  after verifying `cpu.max` is `max 100000`, three unthrottled local runs
  pass. `docker update --cpus 0` did not remove that quota; explicit
  `--cpus=0 --cpu-quota=-1` did. The throttled logs are diagnostics, not a
  matched reproduction of the CI host. Do not increase tolerances or label
  this a kernel clock bug from those logs alone.
- [x] Preserve replay, host/guest FIFO, native-shell, relocated Autotools and
  artifact-storage procedures in implementation-branch documentation commit
  `0ed152954fa8` (documentation only). Kernel validation remains pinned to
  the identical code revision `c0bdf6a0c7a1979e689cadbc6ff095954f93fa69`.

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
- [x] Restore safe private-SWMMU zpoline rewriting in `71ede2f7ae15`, with
  narrow-read test correction `c0bdf6a0c7a1`. The host signal handler queues a
  return PC per task; kernel-context code updates private executable backing
  under mmap/space locks, retaining RX guest permissions and serializing x86
  instruction fetch. Native/shared/guarded backing, cross-page instructions
  and invalid numbers retain SIGSYS fallback. Same fixed kernel, unmodified CI
  getpid binary, alternating three boots per mode: median 200 ns/call versus
  1402 ns SIGSYS across nine million-call averages each. Fresh selftests pass
  repeatedly: zpoline 8/0, SWMMU 38/0/8. Both zpoline modes pass KUnit with
  33 pass / 1 skip / 0 fail. Full CI is still pending.
- [ ] Restore safe native-text rewriting for plain SAS separately. Its physical
  map is anonymous when SWMMU is disabled; do not assume the SWMMU backing-fd
  alias approach also covers native mappings, or re-enable direct RX writes.
- [x] Patch Kirk shell/scheduler elapsed-time sites to monotonic clocks in
  CI `2d0ce905660a`. Backward-clock simulation reproduces a negative duration
  before the change and positive elapsed time afterward against both inspected
  and installed candidate source. Full guest-suite timing remains pending.
  First CI attempt lacked python3 in its LTP build container; `3236b745dd3f`
  installs/verifies that dependency. Replacement full run is `39866498606`,
  kernel `c0bdf6a0c7a1`, LTP `f1fe6fcf68e08eda248186107d77095f550669f5`.
- [ ] Revalidate the full NOMMU LTP matrix with `uml` classification after the
  selected kernel fixes. `mmap04` cross-backend partial MAP_FIXED support/skip,
  `setns02` child-side IPC TCONF propagation remain separate follow-ups.
  Nginx TLS passed a matched direct-window one-worker test; `5968f9355034`
  enables that window in standard SWMMU nginx rows, whose validation is pending.
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

- Lazy SIGSYS zpoline rewriting is restricted to the tested UP SAS paths.
  Private backing ownership, region/mm locks and preemption exclusion protect
  the supported update; they are not a general cross-CPU concurrent instruction
  patching protocol. Keep the multi-thread/SMP execution/fetch issue open until
  explicit concurrency tests and a supported synchronization contract exist.
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

### APK reuse gate — 2026-10-23

Run 39922152442 stopped before builds/tests: there is no APK release for kernel
6d8e67f9be14 and the previous file-count allowlist rejected the new trap layout.
The exact reviewed pair 52a8dfe6b58a -> 6d8e67f9be14 is now accepted with unchanged
aports SHA and checked release/archive manifests. The diff changes kernel-only
state/handlers, docs and selftests; compiler plugin, runtime and UAPI sources are
unchanged. This exception does not authorize future kernel tips or prove LTP
acceptance. Full CI must run; FAIL/BROK/WARN review remains a separate merge gate.

### User-directed CI hold — 2026-10-23

Do not dispatch further CI or merge feature while local LTP FAIL/BROK/WARN and
SIGSEGV investigations remain unresolved. This supersedes the earlier green-only
merge plan. Run 39924156114 was cancelled. sendmsg01 raw syscall candidate is
pushed in thehajime/ltp:hajime/ltp-mmu-native-fixes (6510b8434), validated on native
and MMU ptrace/seccomp, 15 cases each. Full-suite acceptance and result gates remain.
