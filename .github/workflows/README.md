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

Every run builds signed `busybox-nommu` and `musl-nommu` APKs from the pinned
aports revision in GitHub Actions, then installs those verified packages into
the locally built rootfs image. That image is used by the UML smoke and
KUnit steps. Kselftest is temporarily disabled because the NOMMU kselftest
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

The `thehajime/aports` branch `nommu-uml-3.20-swmmu` now contains the BusyBox
`cd -` and `OLDPWD` fix in commit
`46ff44735decafda0d5738097398f77adc6cb312`. The workflow pins this revision.
Exercise the fix in the resulting NOMMU binary. Record the exact aports commit,
package versions, BusyBox fix revision, and published image digest so a
published rootfs can be reproduced and audited. The
`busybox-nommu` and `musl-nommu` package changes are intended for eventual
upstreaming to Alpine; track that work in the root `AGENTS.md` TODO list.

The Buildroot RISC-V image is built locally and the RISC-V LTP matrix entry is
enabled. That job does not publish to GHCR. The LTP workflow still uses its
existing GHCR image as the job container; the locally built candidate is not
shared across jobs. The RISC-V test therefore remains tied to the previously
published image until the LTP job is changed to load a local image artifact.
