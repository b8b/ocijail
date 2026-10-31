Nested podman
=============

An image with podman and the ocijail from this tree, able to create containers
inside itself with the vfs storage driver.

Contents:

- `build.sh` — builds the ocijail binary, then the image (see below);
- `Containerfile` — `pkg install podman`, add the prebuilt ocijail, add the
  configuration, set the entrypoint;
- `containers-bootstrap.conf` — `[engine] runtime = "/usr/bin/false"`, used
  while ocijail does not exist yet, so nothing during the bootstrap can
  silently depend on a container runtime;
- `containers.conf`, `storage.conf` — the *image's* configuration: CNI,
  `runtime = "ocijail"`, `driver = "vfs"` and the `org.freebsd.jail.*`
  annotations every nested container gets;
- its `containers.conf` puts the default podman network on `10.89.0.0/16` (the
  outer podman's default is `10.88.0.0/16`), so nested containers do not share a
  subnet with the outer ones;
- `entrypoint.sh` — enables pf in the container's vnet and installs the CNI NAT
  rules (`table <cni-nat>` plus the `cni-rdr/*` anchors) for the interface with
  the default route;
- `hooks.d/children-max-hook.json` — hook for the *outer* podman, gives the
  container's jail a `children.max` (see below).

Building
--------

Building any image needs a working runtime for its RUN steps, but the image
carries a runtime built from this tree. `build.sh` breaks the cycle:

1. checks that the host is FreeBSD 15.1 or later (`BASE_IMAGE` is pinned to the
   15.1 toolchain image), that `kern.chroot_allow_open_directories` is 2 (buildah
   and the runtime chroot with directories open; the script only reports it,
   setting it is a host job), that inside a jail the parameters needed to run
   containers there are available (a jailed process cannot read its own jail
   parameters, so this probes the kernel by creating throwaway child jails, one
   parameter at a time, and reports the ones that are missing), that buildah is
   installed and that `distfiles/` still has the archives the bazel workspace
   pins;
2. fetches `BASE_IMAGE` and assembles its rootfs as a buildah working container
   (no runtime involved — `containers-bootstrap.conf` points the runtime at
   `/usr/bin/false` so this cannot silently depend on one);
3. mounts devfs into that rootfs, creates a persistent jail on it (sharing this
   network stack, `ip4/ip6=inherit`) and installs `bazel5` and `bash` into it
   with `pkg -j`;
4. copies the workspace, the sources and `distfiles/` into the rootfs and runs
   `bazel build --workspace_status_command=/bin/true //ocijail:ocijail` there
   with `jexec`, then copies the binary out to `ocijail` next to this file
   (`.gitignore`d) and removes the bootstrap container and jail again;
5. builds the image with `buildah bud --runtime ocijail ...`, i.e. the runtime
   that was just built, so the image build only has to `ADD` the binary.

    ./examples/nested-podman/build.sh

To rebuild just the image once `ocijail` exists, call buildah/podman directly
with this directory as the context:

    buildah bud -f examples/nested-podman/Containerfile \
        -t podman examples/nested-podman

Environment: `BASE_IMAGE` (the bootstrap rootfs, default the 15.1 toolchain
image), `BOOTSTRAP_CONTAINER`/`BOOTSTRAP_JAIL` (default `ocijail-bootstrap`),
`IMAGE` (default `podman`), `RUNTIME` (runtime for the image build, default the
binary built in step 4). The image's own base image is the `ARG BASE_IMAGE` in
the `Containerfile` (`ghcr.io/freebsd/freebsd-runtime:15.1`), not the bootstrap
one; override it with `--build-arg BASE_IMAGE=...` when rebuilding the image
directly.

`distfiles/` (the dependencies bazel would otherwise download) is committed, so
`prefetch-deps.sh` is not needed for a fresh checkout; `build.sh` only checks
that the three archives the workspace pins are there and points at it
otherwise.

Running it
----------

The container that hosts the nested podman needs, from the outer podman:

    podman --runtime ./examples/nested-podman/ocijail \
        --hooks-dir examples/nested-podman/hooks.d run --rm -it \
        --annotation org.freebsd.jail.vnet=new \
        --annotation org.freebsd.jail.allow.mount=true \
        --annotation org.freebsd.jail.allow.mount.devfs=true \
        --annotation org.freebsd.jail.allow.mount.fdescfs=true \
        --annotation org.freebsd.jail.allow.mount.nullfs=true \
        --annotation org.freebsd.jail.allow.mount.tmpfs=true \
        --annotation org.freebsd.jail.allow.chflags=true \
        --annotation org.freebsd.jail.allow.extattr=true \
        --annotation org.freebsd.jail.children.max=8 \
        --device /dev/pf \
        podman

Why each one:

- `vnet=new` — the container must own a vnet, otherwise its podman cannot
  create `vnet=new` jails of its own (the image's `containers.conf` asks for
  that for every nested container);
- `allow.mount` + the four `allow.mount.*` — nested ocijail mounts devfs on
  `/dev`, fdescfs on `/dev/fd`, tmpfs on `/dev/shm` and nullfs for bind mounts.
  A child cannot be granted a privilege its parent lacks, so these have to be on
  the container jail itself;
- `allow.chflags`, `allow.extattr` — ocijail requests chflags for every jail it
  creates, and images/storage may want extended attributes;
- `children.max=8` — read by the hook below, not by ocijail;
- `--device /dev/pf` — CNI needs pf in the container's vnet for its NAT rules.
  podman turns a device into a rule for the devfs mount it creates for `/dev`
  (`rule=path pf unhide mode 0600`, see `addDevice()` in
  `pkg/specgen/generate/config_freebsd.go`), which ocijail applies with
  `devfs rule apply` after mounting, so this is what makes the host case work;
  inside a jail ocijail drops the rule ("ignoring devfs option \"rule\" inside
  a jail" with `log-level=debug`) and the inherited ruleset has to expose `pf`
  instead, i.e. the flag is redundant there rather than a problem - podman
  itself still has to be able to see the path, since it stats the device while
  generating the spec, before the runtime runs.
  `--device /dev/vmmctl` works the same way for a nested bhyve.

`allow.raw_sockets` and `set_hostname` are not listed: ocijail sets the former
on every jail and the kernel hands out the latter by default. No `--privileged`
is needed: the runtime drops the devfs `ruleset`/`rule` options when it is
itself in a jail, and the container's `/dev` gets the outer ruleset.

Giving the container a children.max
-----------------------------------

The containers the nested podman creates need a jail of their own, which the
kernel refuses while the container jail has `children.max = 0`. ocijail cannot
set that parameter, so a `createRuntime` hook gives it to the jail from the
`org.freebsd.jail.children.max` annotation. The hook is passed with
`--hooks-dir` on the run command above, nothing has to be installed:

- `hooks.d/children-max-hook.json` is `/bin/sh -c` with the script inline; it
  reads the state ocijail writes to stdin, takes the limit from the
  `org.freebsd.jail.children.max` annotation and the jail from the
  `org.freebsd.jail.jid` that ocijail injects, and runs
  `jail -m jid=<jid> children.max=<n>` in the outer podman's context, after
  `create` and before the container process starts;
- it needs `jq` in the outer jail (`pkg install -y jq`) and a `PATH` that
  reaches `/usr/local/bin` and `/usr/sbin`; the annotation value must be smaller
  than the outer jail's own `children.max`;
- note that ocijail builds the hook's `argv` as `path` followed by every entry
  of `args`, while the OCI spec treats `args` as the full `execv` argv (its
  first entry being the program name). So with ocijail the array must not repeat
  the program name: `"/bin/sh"` with `["-c", "..."]`, not `["sh", "-c", "..."]`,
  which makes `sh` try to open a script called `sh` and exit with status 2.
  Failing hooks are logged by ocijail (`createRuntime hook /bin/sh exited with
  status 2`) but are not fatal;
- the stage list is the top level `"stages"` array, not a `"stage"` key inside
  `"hook"`; podman validates hook files when it reads the directory and rejects
  one without `stages` ("missing required property: stages"), it is then simply
  not applied.

To make it the default instead of passing `--hooks-dir` every time, install the
file into `/usr/local/etc/containers/oci/hooks.d/` and add that directory to
`[engine] hooks_dir` in the outer podman's `containers.conf`.

Notes
-----

- the `pf` kernel module must be loaded on the host; a jail cannot kldload it;
- the container's `/dev` is the devfs mount podman asks for, with
  `ruleset=4` - `devfsrules_jail`, which hides everything but the basic and
  login devices - see the `mounts` in `examples/alpine/config.json`:
  `"options": ["ruleset=4", "rule=path shm unhide mode 1777"]`. `/dev/pf`,
  `/dev/vmm` and `/dev/vmmctl` are not part of that, so where the container
  gets them from depends on where the outer podman runs:
    - inside a jail the runtime drops the mount's `ruleset` and `rule`
      options, because devfs_mount() enforces the jail's own ruleset on any
      devfs it mounts, so the container inherits the ruleset of the jail
      ocijail runs in - that one has to expose the devices. This is the setup
      the example was tested with;
    - on a host the mount is made with ruleset 4 as asked, so the container
      gets exactly that and the devices are missing. Single devices are easy
      to get back, because podman expresses `--device` as a devfs rule for
      that mount: `--device /dev/pf` (above) and `--device /dev/vmmctl` are
      enough. A directory is not: podman walks it and fails with "no devices
      found in /dev/vmm" while no VM nodes exist, so bhyve needs a ruleset
      that exposes `/dev/vmm` - redefine `[devfsrules_jail=4]` in
      `/etc/devfs.rules` and add the rules there (a redefinition replaces the
      one in `/etc/defaults/devfs.rules`, so it has to include
      `devfsrules_hide_all`, `devfsrules_unhide_basic` and
      `devfsrules_unhide_login` again);
- without `/dev/pf` the entrypoint warns, and CNI's ipMasq has no firewall to
  put its NAT rules into, so the containers inside this one have no outbound
  network;
- the default network is created on first use from `default_subnet`
  (`10.89.0.0/16`) and lives in memory, so it is not a file in
  `/usr/local/etc/cni/net.d/` until something writes one; if your outer podman
  uses that subnet too, change `default_subnet` in the image's
  `containers.conf` (or create another network inside the container) so the two
  levels do not collide;
- a nested container that wants to run bhyve needs more than `/dev/vmmctl`.
  Since 15.0 libvmmapi creates and destroys VMs through that one device (the
  `hw.vmm.create`/`hw.vmm.destroy` sysctls are the legacy path), but it still
  opens the per-VM nodes under `/dev/vmm/` (`vm_open()`) and the devmem nodes
  under `/dev/vmm.io/` (`vm_create_devmem()` mmaps them for guest memory), so
  all three paths have to be unhidden - by a ruleset, since `--device` cannot
  add a directory (`/dev/vmm` and `/dev/vmm.io` are empty until a VM exists,
  and podman gives up with "no devices found"). It also needs `allow.vmm` at
  every level: jail_set() refuses a permission the parent jail does not have,
  so the container needs `--annotation org.freebsd.jail.allow.vmm=true` here
  and its own jail needs the flag as well;
- the nested podman inherits ocijail, which also cleans up after such VMs:
  killing a bhyve does not destroy its VM before 15.2, so a `delete` unmounts
  the interfaces, destroys the VMs the container left behind and removes its
  child jails;
- one level of nesting is what this supports: the containers inside the
  container get `children.max = 0` again, so they cannot create jails
  themselves.
