#!/usr/bin/env -S bash ../.port_include.sh
port=libfuse
version=3.10.5
useconfigure=true
configopts="--cross-file ../cross_file-$SERENITY_ARCH.txt"
files="https://github.com/libfuse/libfuse/archive/refs/tags/fuse-$version.tar.gz libfuse-$version.tar.gz e73f75e58da59a0e333d337c105093c496c0fd7356ef3a5a540f560697c9c4e6"
auth_type=sha256
workdir=libfuse-fuse-$version

configure() {
    run meson _build $configopts
}

build() {
    run ninja -C _build
}

install() {
    export DESTDIR=$SERENITY_BUILD_DIR/Root
    run meson install -C _build
}
