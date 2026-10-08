# UML development CI and images

Keep these workflows on the `ci/uml-nommu` branch. They are development
infrastructure for UML NOMMU, SAS, and SWMMU work; do not include them in
patches sent upstream to `uml/next`.

## Running CI

Push-triggered runs only happen on branches that contain the workflow file.
For example, pushing to `feature-swmmu-nommu` will not start this workflow
unless that branch has merged or copied `.github/workflows/ci.yml`. To test it
without changing the topic branch, dispatch the workflow from `ci/uml-nommu`
and set `kernel_ref` to the branch, tag, or commit. The CI job checks out the
selected kernel plus the canonical support files from `ci/uml-nommu`.

Every run builds signed NOMMU APK profiles from the pinned aports revision in
GitHub Actions. MMU UML keeps Alpine's standard userspace; `um-nommu` uses
uninstrumented `SWMMU=0` APKs with `CONFIG_NOMMU_SWMMU` disabled, and
`um-nommu-sas` uses instrumented `SWMMU=1` APKs built with the selected kernel
ref's GCC plugin and UAPI. Both profiles are built and installed into their
matching local rootfs images. Plain x86_64 NOMMU UML does not support the
standard `BINFMT_ELF` loader, but its FDPIC loader accepts `ET_DYN` PIE binaries
without FDPIC-specific marking when `MMU=n`. CI boots the plain NOMMU image and
checks that its userspace init script starts. Its kernel is built without the
SWMMU implementation. SAS requires `CONFIG_NOMMU_SWMMU=y`; the workflow asserts
that setting for the SAS profile. SAS and MMU UML also run guest boot tests. For
the
SWMMU image, APK install scripts are skipped because the
instrumented guest BusyBox cannot run on the build host; a static helper from
the same aports build recreates its applet links. Kselftest is temporarily disabled because the NOMMU kselftest
suite is not present in the tested kernel tree and the generic UML run takes
too long. Re-enable focused kselftest coverage after the NOMMU tests are
upstream and the matching test target is available in this branch. Normal CI
does not publish the UML images.

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

The UML and Buildroot RISC-V images are built locally, saved as workflow
artifacts, and loaded by benchmark and LTP jobs. No test job pulls a GHCR image;
GHCR is used only when `publish_images` is explicitly enabled. The RISC-V LTP
build and test jobs run in the Buildroot image from that run; its cross
toolchain is not copied from an older published image.

The LTP runner temporarily excludes `ptrace06` from `um-mmu` and
`perf_event_open03` from `um-nommu-seccomp` and `um-nommu-sas-seccomp`. The
root `AGENTS.md` tracks removing these skips after the failures are diagnosed.

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
and in the Actions artifacts for their respective runs.
