# syntax=docker/dockerfile:1
FROM alpine:3.20.3

ARG MODE=
ARG USERSPACE_PROFILE=alpine
ADD vmlinux /root/.
ADD inittab /etc/inittab
RUN mkdir /etc/init.d
ADD rcS /etc/init.d/.
RUN if [ "${USERSPACE_PROFILE}" = alpine ]; then \
      sed "s/\'/\"/g" /etc/profile > /.profile; \
    else \
      printf '%s\n' 'export PATH=/home:/sbin:/usr/sbin:/bin:/usr/bin' \
        'export TERM=linux' > /etc/profile && \
      cp /etc/profile /.profile; \
    fi


ADD bench.sh /
ADD iperf3.sh /root/
ADD netperf-bench.sh /root/
ADD iperf3.static /root/
ADD do_getpid /root/.
ADD clone /root/.
ADD futex /root/.
ADD nullptr /root/.
ADD ctest /root/.
ADD stackp /root/.
ADD init_array /root/.
ADD entropy /root/.
ADD locale /root/.
ADD vdso_test /root/.
ADD pthread /root/.
ADD Makefile /root/.
ADD lmbench2/ /lmbench2
ADD lmbench_run.sh /lmbench2/bin/x86_64-linux-gnulibc1/
ADD netperf /root/.
ADD lib/ /lib

RUN apk update && apk add \
	acl curl jq keyutils libaio libacl libcap libselinux libsepol libtirpc utmps-libs \
	numactl openssl py3-msgpack btrfs-progs e2fsprogs xfsprogs \
	dosfstools parted util-linux make iperf3 python3 bash git && \
	adduser -D -g "Unprivileged LTP user" ltp && \
	apk del btrfs-progs
RUN if [ "${USERSPACE_PROFILE}" = alpine ] || \
      [ "${USERSPACE_PROFILE}" = nommu-plain ]; then \
      apk add coreutils; \
    fi

# for ifupdown
RUN mkdir -p /etc/network
ADD interfaces /etc/network/.

# XXX
RUN mkdir -p /usr/share/udhcpc
ADD default.script /usr/share/udhcpc/.
RUN chmod +x /usr/share/udhcpc/default.script

RUN --mount=type=bind,source=aports-artifacts,target=/tmp/aports-artifacts,ro \
    case "${USERSPACE_PROFILE}" in \
      alpine) ;; \
      nommu-plain|nommu-swmmu) \
        cp /tmp/aports-artifacts/keys/*.rsa.pub /etc/apk/keys/ && \
        cp /tmp/aports-artifacts/${USERSPACE_PROFILE}/tools/busybox-host /tmp/busybox-host && \
        chmod 755 /tmp/busybox-host && \
        apk add --no-cache --no-scripts \
          /tmp/aports-artifacts/${USERSPACE_PROFILE}/packages/busybox-nommu-*.apk \
          /tmp/aports-artifacts/${USERSPACE_PROFILE}/packages/musl-nommu-*.apk && \
        /tmp/busybox-host --install -s ;; \
      *) echo "unknown USERSPACE_PROFILE: ${USERSPACE_PROFILE}" >&2; exit 1 ;; \
    esac

ENTRYPOINT ["/root/vmlinux", "vec0:transport=raw,ifname=eth0,depth=128,gro=1", "root=/dev/root", "rootflags=/", "rootfstype=hostfs", "rw", "mem=2048m", "loglevel=0", "init=/sbin/init"]
