# UML development CI and images

Keep these workflows on the `ci/uml-nommu` branch. They are development
infrastructure for UML NOMMU, SAS, and SWMMU work; do not include them in
patches sent upstream to `uml/next`.

## Running CI

Push-triggered runs only happen on branches that contain the workflow file.
For example, pushing to `feature-swmmu-nommu` will not start this workflow
unless that branch has merged or copied `.github/workflows/ci.yml`. To test it
without changing the topic branch, dispatch the workflow from `ci/uml-nommu`
and set `kernel_ref` to the branch, tag, or full commit SHA. Short SHA prefixes
are not accepted by the checkout action. The CI job checks out the selected
kernel plus the canonical support files from `ci/uml-nommu`.

The `thehajime/aports` workflow `publish-nommu-apks.yml` builds signed NOMMU
APK profiles on demand and publishes a prerelease asset keyed by the exact
Linux and aports commits. Publish the pair selected by `ci.yml` before running
the UML matrix; the build job downloads that matching asset. MMU UML keeps
Alpine's standard userspace; `um-nommu` uses
uninstrumented `SWMMU=0` APKs with `CONFIG_NOMMU_SWMMU` disabled, and
`um-nommu-sas` uses instrumented `SWMMU=1` APKs built with the selected kernel
ref's GCC plugin and UAPI. The experimental `um-nommu-plain-sas` profile
combines the plain `SWMMU=0` APKs with `CONFIG_UML_NOMMU_SAS=y`, keeps
`CONFIG_NOMMU_SWMMU=n`, and runs without UML time-travel mode. Its LTP row is
the normal syscall suite built with `NOMMU=1`; LTP compile guards and runtime
`TCONF` results handle tests unsupported without `fork()`. These profiles are
built and installed into their
matching local rootfs images. Plain x86_64 NOMMU UML does not support the
standard `BINFMT_ELF` loader, but its FDPIC loader accepts `ET_DYN` PIE binaries
without FDPIC-specific marking when `MMU=n`. CI boots the plain NOMMU image and
checks that its userspace init script starts. Its kernel is built without the
SWMMU implementation. The SWMMU SAS profile requires
`CONFIG_NOMMU_SWMMU=y`; the plain SAS profile explicitly asserts the opposite.
SAS and MMU UML also run guest boot tests. For the
SWMMU image, APK install scripts are skipped because the
instrumented guest BusyBox cannot run on the build host; a static helper from
the same aports build recreates its applet links. NOMMU images install that
non-instrumented control binary and its applet links under
`/opt/uml-host-tools`; Actions orchestration uses only that directory, while
the guest rootfs continues to use its profile-specific `/bin` and `/sbin`.
This separation prevents the UML guest syscall interception from being
required by container-control `sh`, `sleep`, or `readlink` processes. Those
processes run outside UML before, during, and after the guest. Test JSON, HTML
and console output are generated or copied to the shared artifact directory
before the guest halts. The post-halt host-control calls only validate those
shared paths and finish the outer container orchestration; they do not create
guest test results after shutdown. The focused
direct SWMMU kselftest runs only in the `um-nommu-sas` build row. The generic
UML kselftest run remains disabled because it takes too long. Normal CI does
not publish the UML images.

To publish the tested images, use **Actions → ci.yml → Run workflow** on
`ci/uml-nommu` and enable `publish_images`. The publish job only starts after
the UML build and test matrix succeeds. It packages the tested image build
contexts as workflow artifacts, then publishes the stable versioned tags to
GHCR. Do not enable this option for experimental branches whose images should
not replace the documented baseline.

For GitHub to offer this workflow in Actions, `ci/uml-nommu` must be the
repository's default branch. Set that in GitHub repository settings; a Git
push cannot change the default branch. The image tags used by
`Documentation/virt/uml/nommu-uml.rst` should only be updated from a validated
kernel and rootfs combination.

The nginx comparison runs only on a manual `ci.yml` dispatch; it adds no
dispatch inputs and uses the selected kernel ref plus the pinned aports commit
recorded in each UML rootfs manifest. A native runner baseline is measured in a
separate folded matrix while the UML rootfs and kernels build. APKs are built
once. MMU, plain NOMMU, and SWMMU rootfs artifacts are each prepared once and
shared across three seccomp UML profiles (`um-mmu-seccomp`, `um-nommu-seccomp`,
and NOMMU SAS with `zpoline=0`). PTRACE and `zpoline=1` are excluded from nginx
benchmarking. Every supported UML profile runs worker counts 1/4 and wrk
connections 4/16/64 from a separate client container. The native baseline
measures the same matrix on the runner and records its available CPU count. MMU
profiles cross one and four online CPUs with both worker counts and all client
connection counts. Plain NOMMU seccomp compares a one-CPU baseline with a
four-CPU `CONFIG_SMP=y` kernel; NOMMU SAS remains single CPU because UML's NOMMU
SAS configuration disables SMP. The top-level matrix
calls the reusable workflow per leg so GitHub can group the legs at the caller.
The run records the online CPU list. Each leg
uploads raw logs and `measurement.json`; `bench-result` aggregates profile
data and uploads `nginx-aggregate-plot` with JSON, CSV, profile plots, and
SWMMU rootfs diagnostics. HTTP/TLS smoke checks must pass, and a wrk timeout
or socket error fails the leg. LTP build and test jobs are enabled again. SAS
LTP remains under investigation by Task A, so its result must be read as
diagnostic evidence rather than a resolved or stable test outcome.
Manual diagnostic dispatch also skips benchmark-2 iperf3/netperf jobs;
lmbench benchmark-1 remains enabled.
The SWMMU package/rootfs build runs as root in the Alpine chroot so it can
create the unprivileged `abuild` user, then restores runner ownership before
uploading diagnostics.

## Root filesystem package source

The Dockerfile receives the APKs and generated public signing key as a
workflow artifact. The base image verifies package signatures during
installation. The temporary GitLab CI artifact is no longer used.

The `thehajime/aports` branch `nommu-uml-3.20-swmmu` contains the BusyBox
`cd -` and `OLDPWD` fix in commit
`46ff44735decafda0d5738097398f77adc6cb312`; the workflow pins its later
revert commit `f72f56c2be4f6c07be9f6adadcbe7f1efa76af93`, which preserves those
fixes without claiming a conventional static ELF is executable on UML NOMMU.
Exercise the fix in the resulting NOMMU binary. Record the exact aports commit,
package versions, BusyBox fix revision, and published image digest so a
published rootfs can be reproduced and audited. The
`busybox-nommu` and `musl-nommu` package changes are intended for eventual
upstreaming to Alpine; track that work in the root `AGENTS.md` TODO list.

The UML images are built in each CI run and passed to benchmark and LTP jobs as
workflow artifacts. The RISC-V Buildroot image is maintained separately by
`build-riscv-image.yml`, which is a manual workflow with its own kernel ref,
independent of the UML SWMMU kernel ref. For now it uses the previously proven
`zpoline-nommu-v6.10` branch; updating to a newer RISC-V NOMMU kernel is tracked
in `AGENTS.md`. The workflow updates the LTP fork's Buildroot Dockerfile to the
selected exact commit and kernel version, then publishes an immutable GHCR tag.
The optional `riscv-nommu` matrix entry pins that image tag and builds its LTP
test programs once in the `build_ltp_riscv` prerequisite job. Both jobs are
disabled by default because the QEMU LTP run is unusually slow; enable them
with the `run_riscv_ltp` workflow-dispatch input when explicitly needed. The
report labels the omitted row as disabled. The QEMU test row downloads the
resulting install-tree artifact, matching the build-once model used by the
other LTP profiles. Update the image tag in `ci.yml` only after publishing and
validating a replacement. The RISC-V comparison image can be refreshed less
often than the main kernel CI. Its uClibc NOMMU profile skips
`futex_wait03`: the pthread test reports a broken result and leaves Kirk's
QEMU channel waiting for the next shell prompt. The same full syscall suite
completes and writes its report when that unsupported case is omitted.

The native lmbench, getpid, iperf3, and netperf measurements run in the
independent `checkpatch` job and are uploaded as `test-result-native`.
`bench-result` only downloads, aggregates, and plots measurements and test
results after the benchmark/LTP jobs finish. The LTP jobs do not currently use
ccache. The repository already has about 10.4 GB of Actions cache entries,
mostly BuildKit layers and per-run UML compiler caches, so additional LTP
compiler caches are deferred until cache retention or quota is reviewed.

The LTP runner temporarily excludes `ptrace06` from `um-mmu` and
`perf_event_open03` from `um-nommu-seccomp` and `um-nommu-sas-seccomp`. The
root `AGENTS.md` tracks removing these skips after the failures are diagnosed.
The plain NOMMU SAS row runs the normal NOMMU LTP syscall suite on manual
workflow dispatch. The LTP artifact is built with `NOMMU=1`, so compile guards
and runtime `TCONF` results handle cases unsupported without `fork()`.
Configuration skips remain visible in the JSON and HTML reports; other nonzero
outcomes fail the row. The row reuses the plain NOMMU LTP and userspace
artifacts and does not use SWMMU-instrumented packages.
All time-travel build rows pass `time-travel` to both image boot tests and to
KUnit. Their KUnit fragments independently enable time-travel and disable the
incompatible RAID6 benchmark. Plain NOMMU KUnit fragments explicitly disable
SWMMU rather than inheriting its `default y` Kconfig value.

## Test and benchmark result reports

`bench-result` writes a versioned JSON snapshot and posts a comparison to
`thehajime/linux-private#4`. The snapshot keeps benchmark measurements as
records with `name`, `target`, `value`, `unit`, and optional `labels`, for
example an iperf3 record uses `labels.direction` and a netperf record uses
`labels.test` and `labels.payload_bytes`. To add another benchmark, normalize
its output into these records in `result-report.py`; the comparison and report
code will generate stable keys from the record fields automatically. Existing
`.dat` benchmark output is currently adapted into this schema. LTP and KUnit
are collected from their JSON output, while kselftest TAP output is normalized
to named case/status records. Snapshot schema version 2 remains able to read
older version 1 benchmark maps for comparison.

Benchmark charts and the structured snapshot are published to the repository's
GitHub Pages site by the `bench-result` job. Pages must be enabled in
**Settings → Pages** with **Build and deployment → Source** set to **GitHub
Actions**. The job requests `pages: write` and `id-token: write`, so no Imgur
client ID or other publishing secret is needed. Each successful deployment
replaces the site's current report; historical comparisons remain in issue #4
and in the Actions artifacts for their respective runs. Runs from experimental
workflow branches use the unprotected `ci-report-preview` environment: they
still create the report artifact and issue comment, but only `ci/uml-nommu`
deploys the generated site to the protected `github-pages` environment.
