# UML development CI and images

Keep these workflows on the `ci/uml-nommu` branch. They are development
infrastructure for UML NOMMU, SAS, and SWMMU work; do not include them in
patches sent upstream to `uml/next`.

## Running CI

The workflow runs for pushes to branches that contain this workflow and
builds/tests the pushed kernel revision. To validate a topic branch without
copying the workflow into it, dispatch the workflow from `ci/uml-nommu` and set
`kernel_ref` to the topic branch, tag, or commit. It builds the root filesystem
image locally and uses that image for the UML smoke and test steps. Normal CI
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

The current `Dockerfile` still downloads `busybox-nommu` and `musl-nommu` from
a temporary GitLab CI artifact. Such artifacts expire and the pinned URL may
return 404, so the image build and publication remain blocked until this input
is replaced. Build these APKs from a pinned `thehajime/aports` revision in
GitHub Actions, retain normal checksum and package-signature verification, and
pass the resulting verified APKs and signing key into the Docker build. Do not
publish an image when either APK is unavailable or unverified.

The `thehajime/aports` branch `nommu-uml-3.20-swmmu` now contains the BusyBox
`cd -` and `OLDPWD` fix in commit
`46ff44735de`. Exercise the fix in the resulting NOMMU binary. Record the exact
aports commit, package versions, BusyBox fix revision, and published image
digest so a published rootfs can be reproduced and audited. The
`busybox-nommu` and `musl-nommu` package changes are intended for eventual
upstreaming to Alpine; track that work in the root `AGENTS.md` TODO list.

The unfinished Buildroot RISC-V image build and its LTP matrix entry are
disabled. They must not publish to GHCR until the image is complete and
validated.
