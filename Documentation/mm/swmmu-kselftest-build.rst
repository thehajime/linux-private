=============================
Building SWMMU NOMMU kselftests
=============================

Build the NOMMU selftest directly from ``tools/testing/selftests/nommu``. The
test is not under ``tools/testing/selftests/mm`` in this tree. This procedure
assumes a configured UML NOMMU SWMMU kernel and a musl cross compiler that uses
the same GCC version as the GCC plugin.

Export the kernel UAPI headers and build the GCC plugin with the matching host
compiler:

.. code-block:: sh

   mkdir -p build-kselftest
   make ARCH=um O=build headers_install \
           INSTALL_HDR_PATH=build-kselftest/uapi
   make -C tools/testing/selftests/nommu \
           SWMMU_BUILD_PLUGIN=1 SWMMU_HOSTCXX=g++ \
           SWMMU_PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           swmmu-plugin

Build a musl static PIE with all-access instrumentation and direct-return scalar
loads:

.. code-block:: sh

   mkdir -p build-kselftest/nommu
   make -B -C tools/testing/selftests/nommu \
           OUTPUT="$PWD/build-kselftest/nommu" \
           CC=x86_64-linux-musl-gcc \
           KHDR_INCLUDES="-I$PWD/build-kselftest/uapi/include" \
           'USERCFLAGS=-include sys/prctl.h' \
           USERLDFLAGS=-static-pie \
           SWMMU_PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           SWMMU_ALL_ACCESS=1 SWMMU_RETURN_VALUE_LOAD=1 all

The ``SWMMU_RETURN_VALUE_LOAD`` option is experimental. It makes plugin-lowered
scalar loads call ``nommu_swmmu_load_return_value``. That runtime helper must
issue the load syscall directly so 64-bit data values in the syscall errno
range are not converted to ``errno``. The selftest runtime implements this
helper and checks a value in that range. Store and bulk memory calls keep their
existing ABI.

The installed or standalone test binary must run on a kernel that implements
``NOMMU_SWMMU_ACCESS_RETURN_VALUE``. For a guest test, copy the binary into the
test root filesystem, use an init script that records its exit code, and check
both the guest result marker and UML host exit status. Keep the kernel build,
cross-build, guest root filesystem, and their logs in separate output paths.

The plugin lowering can be verified without booting a guest:

.. code-block:: sh

   make -C scripts/gcc-plugins/tests \
           PLUGIN="$PWD/build-kselftest/swmmu_plugin.so" \
           OUTPUT="$PWD/build-kselftest/plugin-tests" \
           all-access-return-value
