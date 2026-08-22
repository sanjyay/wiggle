FROM debian:bookworm-slim@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241

# Debian Snapshot is immutable by timestamp. APT verifies the snapshot's
# signed Release metadata and the package hashes referenced by that metadata.
RUN sed -i \
      -e 's|http://deb.debian.org/debian-security|http://snapshot.debian.org/archive/debian-security/20260820T000000Z|' \
      -e 's|http://deb.debian.org/debian|http://snapshot.debian.org/archive/debian/20260820T000000Z|' \
      /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
      gcc=4:12.2.0-3 \
      gcc-12=12.2.0-14+deb12u1 \
      cpp-12=12.2.0-14+deb12u1 \
      libgcc-12-dev=12.2.0-14+deb12u1 \
      libgcc-s1=12.2.0-14+deb12u1 \
      libc6=2.36-9+deb12u14 \
      libc6-dev=2.36-9+deb12u14 \
      linux-libc-dev=6.1.180-1 \
      libevdev2=1.13.0+dfsg-1 \
      libevdev-dev=1.13.0+dfsg-1 \
      pkg-config=1.8.1-1 \
      pkgconf=1.8.1-1 \
      pkgconf-bin=1.8.1-1 \
      binutils=2.40-2 \
      binutils-common=2.40-2 \
      binutils-x86-64-linux-gnu=2.40-2 \
    && rm -rf /var/lib/apt/lists/*

COPY build-helper.sh /usr/local/bin/build-wiggle-monitor

ENV LC_ALL=C \
    LANG=C \
    TZ=UTC \
    SOURCE_DATE_EPOCH=1787184000 \
    ZERO_AR_DATE=1

ENTRYPOINT ["/usr/local/bin/build-wiggle-monitor"]
