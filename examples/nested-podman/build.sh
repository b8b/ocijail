#!/bin/sh
#
# Build the nested-podman image.
#
# The image carries a runtime built from this tree, and building any image needs
# a working runtime for its RUN steps - so the two halves are ordered around
# that: first ocijail is built without a container at all (fetch the base image
# with buildah, assemble its rootfs, run bazel in a jail on it), then the image
# is built with that freshly built runtime and only has to add the binary.
#
# Environment:
#   BASE_IMAGE            image for the bootstrap rootfs, i.e. what bazel runs
#                         in (default: the 15.1 toolchain image); the image
#                         itself is built FROM the Containerfile's ARG
#   BOOTSTRAP_CONTAINER   buildah working container holding the base rootfs
#   BOOTSTRAP_JAIL        jail name used to work on that rootfs
#   IMAGE                 tag for the resulting image (default: podman)
#   RUNTIME               runtime used for the image build (default: the binary
#                         this script built)
set -eu

: "${BASE_IMAGE:=ghcr.io/freebsd/freebsd-toolchain:15.1}"
: "${BOOTSTRAP_CONTAINER:=ocijail-bootstrap}"
: "${BOOTSTRAP_JAIL:=ocijail-bootstrap}"
: "${IMAGE:=podman}"

here=$(realpath "$(dirname "$0")")
tree=$(dirname "$(dirname "$here")")
cd "$tree"

: "${RUNTIME:=$here/ocijail}"

log() { echo "build.sh: $*" >&2; }
die() { echo "build.sh: $*" >&2; exit 1; }

on_exit() {
    # The bootstrap jail and its working container are only dropped once the
    # build in them succeeded; on failure they are left in place so the rootfs
    # and the bazel output can be looked at.
    status=$?
    if [ "$status" -eq 0 ] || [ "${bootstrapped:-0}" != 1 ]; then
        return
    fi
    log "build failed; left $BOOTSTRAP_JAIL on $BOOTSTRAP_CONTAINER in place"
    log "remove them with: jail -r $BOOTSTRAP_JAIL; umount ${root:-<rootfs>}/dev;" \
        "buildah rm $BOOTSTRAP_CONTAINER"
}
trap on_exit EXIT

check_os() {
    osreldate=$(sysctl -n kern.osreldate)
    if [ "$osreldate" -lt 1501000 ]; then
        die "FreeBSD 15.1 or later required (kern.osreldate is $osreldate)"
    fi
}

check_chroot() {
    # buildah and the OCI runtime call chroot(2) with directory descriptors
    # still open, which only works with the value 2. It cannot be set from a
    # jail, and it is a host setting, so only report it.
    value=$(sysctl -n kern.chroot_allow_open_directories)
    [ "$value" = 2 ] ||
        die "kern.chroot_allow_open_directories is $value, must be 2" \
            "(set it on the host: sysctl kern.chroot_allow_open_directories=2," \
            "and keep it in /etc/sysctl.conf; this script will not change it)"
}

check_jail() {
    if [ "$(sysctl -n security.jail.jailed)" != 1 ]; then
        return
    fi

    # A jailed process cannot query its own parameters, so ask the kernel: a
    # child jail can only be created with parameters our jail has, and only if
    # children.max is not zero. Probing one parameter at a time tells us exactly
    # which ones are missing.
    check_name=ocijail-build-check
    jail -r "$check_name" >/dev/null 2>&1 || true

    probe() {
        if jid=$(jail -ci name="$check_name" path=/ persist "$@" 2>&1); then
            jail -r "$jid" >/dev/null 2>&1 || true
            return 0
        fi
        probe_error=$jid
        return 1
    }

    if ! probe; then
        die "inside a jail that cannot create jails at all:" \
            "$probe_error (children.max is 0, or the jail is not allowed to" \
            "create child jails)"
    fi

    missing=
    for allow in allow.mount allow.mount.devfs allow.mount.fdescfs \
            allow.mount.nullfs allow.mount.tmpfs allow.chflags \
            allow.raw_sockets; do
        probe "$allow" || missing="${missing:+$missing }$allow"
    done
    if [ -n "$missing" ]; then
        die "inside a jail that lacks the parameters a container build needs:" \
            "$missing (add them to the jail's configuration, see" \
            "examples/nested-podman/README.md for the set that is used there)"
    fi

    probe allow.mount.zfs ||
        log "note: no allow.mount.zfs; needed if buildah uses the zfs storage driver"
}

check_buildah() {
    command -v buildah >/dev/null 2>&1 ||
        die "buildah not found (pkg install buildah)"
}

check_distfiles() {
    # The workspace builds with --distdir=distfiles and repository downloads
    # disabled, so these archives are the only source for its dependencies. They
    # are committed, but a partial checkout or an rsync with exclusions is easy
    # to end up with.
    for archive in main.zip v2.3.2.zip include.zip; do
        [ -f "distfiles/$archive" ] ||
            die "distfiles/$archive missing (run ./prefetch-deps.sh)"
    done
}

clean_bootstrap() {
    jail -r "$BOOTSTRAP_JAIL" >/dev/null 2>&1 || true
    if root=$(buildah mount "$BOOTSTRAP_CONTAINER" 2>/dev/null); then
        umount "$root/dev" >/dev/null 2>&1 || true
    fi
    buildah rm "$BOOTSTRAP_CONTAINER" >/dev/null 2>&1 || true
}

bootstrap_rootfs() {
    # Fetch the base image and assemble its rootfs as a buildah working
    # container. containers-bootstrap.conf points the OCI runtime at
    # /usr/bin/false so that this cannot silently depend on a runtime that does
    # not exist yet.
    export CONTAINERS_CONF="$here/containers-bootstrap.conf"

    clean_bootstrap
    buildah from --pull=always --quiet --name "$BOOTSTRAP_CONTAINER" \
        "$BASE_IMAGE" >/dev/null

    root=$(buildah mount "$BOOTSTRAP_CONTAINER")
    [ -n "$root" ] && [ -d "$root" ] ||
        die "cannot mount $BOOTSTRAP_CONTAINER (with the zfs storage driver the" \
            "jail needs allow.mount.zfs)"

    # pkg inside the bootstrap rootfs needs working resolvers.
    cp /etc/resolv.conf "$root/etc/resolv.conf"

    bootstrapped=1
    log "bootstrap rootfs at $root"
}

prepare_jail() {
    # bazel, and the packages it installs, are run in a jail on the bootstrap
    # rootfs: pkg -j installs into that rootfs, and the build itself runs there
    # with jexec. Mounting devfs from here keeps allow.mount off the jail.
    mount -t devfs devfs "$root/dev"
    # ip4/ip6=inherit is the jail equivalent of podman --network=host: the jail
    # shares this network stack (vnet is inherited by default) and may use all
    # of its addresses. Without it a non-vnet jail is created restricted with an
    # empty address list, i.e. without any IPv4 at all.
    jail -c name="$BOOTSTRAP_JAIL" path="$root" persist ip4=inherit ip6=inherit
    pkg -j "$BOOTSTRAP_JAIL" install -y bazel5 bash

    log "bootstrap jail $BOOTSTRAP_JAIL ready on $root"
}

copy_sources() {
    # Everything the bazel build reads: the workspace, the sources and the
    # vendored dependency archives (the workspace is off the network).
    src="$root/usr/src/ocijail"
    mkdir -p "$src"
    cp -a WORKSPACE BUILD .bazelrc ocijail third_party distfiles tools "$src/"
    log "sources copied to $src"
}

build_ocijail() {
    src=/usr/src/ocijail

    jexec "$BOOTSTRAP_JAIL" /usr/bin/env \
        HOME=/root PATH=/usr/local/bin:/usr/bin:/bin \
        /bin/sh -c "cd $src && exec bazel build --workspace_status_command=/usr/bin/true //ocijail:ocijail"

    # bazel's convenience symlinks point into the output base with absolute
    # paths, which are only valid inside the jail, so resolve the binary there
    # and copy the plain file out through the rootfs.
    jexec "$BOOTSTRAP_JAIL" /bin/sh -c \
        "cp $src/bazel-bin/ocijail/ocijail /root/ocijail"
    [ -f "$root/root/ocijail" ] ||
        die "bazel did not produce $src/bazel-bin/ocijail/ocijail"

    install -m 755 "$root/root/ocijail" "$here/ocijail"
    log "built $here/ocijail"
}

build_image() {
    # The image only adds the binary built above; its RUN step needs a runtime,
    # which is the one we just built unless RUNTIME says otherwise. Its base
    # image comes from the Containerfile's ARG, not from BASE_IMAGE.
    buildah bud --runtime "$RUNTIME" \
        -f "$here/Containerfile" -t "$IMAGE" "$here"
    log "built image $IMAGE"
}

check_os
check_chroot
check_jail
check_buildah
check_distfiles

log "bootstrap base image $BASE_IMAGE"
bootstrap_rootfs
prepare_jail
copy_sources
build_ocijail
# Only reached when the build above succeeded (set -e), so this is the point to
# throw the bootstrap rootfs away.
clean_bootstrap
build_image

# The command goes to stdout, unprefixed, so it can be copied; everything else
# this script says is on stderr.
log "run the image with:"
cat <<EOT
podman --runtime $RUNTIME --hooks-dir $here/hooks.d run --rm -it \\
    --annotation org.freebsd.jail.vnet=new \\
    --annotation org.freebsd.jail.allow.mount=true \\
    --annotation org.freebsd.jail.allow.mount.devfs=true \\
    --annotation org.freebsd.jail.allow.mount.fdescfs=true \\
    --annotation org.freebsd.jail.allow.mount.nullfs=true \\
    --annotation org.freebsd.jail.allow.mount.tmpfs=true \\
    --annotation org.freebsd.jail.allow.chflags=true \\
    --annotation org.freebsd.jail.allow.extattr=true \\
    --annotation org.freebsd.jail.children.max=8 \\
    --device /dev/pf \\
    $IMAGE
EOT
