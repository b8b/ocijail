Podman in a jail
================

A persistent host jail that runs podman with [ocijail](../../README.md) as its
runtime, so the containers podman starts are child jails of this jail and get
their own vnets nested inside the jail's vnet.  Contrast with
[`examples/nested-podman`](../nested-podman/README.md), which runs podman
inside a container; this one is the host-side jail equivalent.

Contents:

- `podman.conf` - the jail(8) configuration, installed as
  `/etc/jail.conf.d/podman.conf`;
- `jail.rc.conf` - the jail's own `/etc/rc.conf`, with its side of the network,
  `zfs_enable` and `podman_enable`;
- `containers.conf`, `storage.conf` - the *jail's* podman configuration,
  installed under `/usr/local/etc/containers/` in the jail;
- `pf.conf` - the jail's own pf rules, installed as `/jail/podman/etc/pf.conf`.
  The CNI plugins only manage the `<cni-nat>` table and the `cni-rdr/*`
  anchors, the NAT rule itself has to be provided.

How it fits together
--------------------

Traffic goes through two NAT instances and two bridges:

    container --(jail's CNI bridge, 10.88.0.0/16)--> jail pf --> epair0b
        --(host bridge0, 10.99.0.0/24)--> host pf --> host uplink

- the jail owns a vnet (`vnet = "new"`), because podman gives every container a
  vnet of its own and a jail can only grant a child a permission it has itself;
- the host creates the epair pair and puts `epair0a` on `bridge0`
  (`exec.prestart`); jail(8) hands `epair0b` to the jail (`vnet.interface`)
  after `exec.created` and before `exec.start`.  Everything on the jail's side
  - the interface address, the default route, forwarding, pf - comes from the
  jail's own `/etc/rc.conf` (`ifconfig_epair0b`, `defaultrouter`,
  `gateway_enable`, `pf_enable`), whose rc scripts run in vnet jails;
- the jail's pf (a vnet jail has its own pf, separate from the host's) NATs the
  container subnet to the jail's epair address, and the host's pf NATs the jail
  subnet to the uplink;
- podman's storage lives on `zroot/podman`.  The dataset is created once on the
  host, unmounted, with `-o mountpoint=/var/db/containers` - the path the jail
  sees, which is the parent of podman's default graphroot.  `zfs.dataset`
  attaches it to the jail at start, and the jail's rc mounts it
  (`zfs_enable=YES`).  `jailed=on` lets the jail manage the layer datasets the
  zfs storage driver creates below it, and also makes the host's
  `zfs mount -a` skip it, so it is only ever mounted inside the jail;
- `devfs_ruleset = 5` is `devfsrules_jail_vnet` from
  `/etc/defaults/devfs.rules`: the usual jail devices plus `zfs` and `pf`.
  ocijail drops devfs `ruleset`/`rule` options inside a jail, so the container's
  `/dev` is mounted with this ruleset and sees the same devices;
- `children.max = 100` is what allows podman to create container jails at all.

Host requirements
-----------------

- FreeBSD 15.1 or later (`kern.osreldate >= 1501000`); this is the version the
  tree is developed and tested against;
- `kern.chroot_allow_open_directories=2` - buildah, if you build images inside
  the jail, chroots with directories open; running containers with podman and
  ocijail does not need it.  This is a host setting, put it in
  `/etc/sysctl.conf`;
- `VIMAGE` in the kernel (GENERIC has it), and `if_bridge`, `if_epair` and `pf`
  available; on a modular kernel put them in `kld_list` in `/etc/rc.conf`:

      kld_list="if_bridge if_epair pf"

- IP forwarding and a pf rule for the jail subnet.  `/etc/rc.conf`:

      cloned_interfaces="bridge0"
      ifconfig_bridge0="inet 10.99.0.1/24 up"
      gateway_enable="YES"
      pf_enable="YES"
      jail_enable="YES"
      jail_list="podman"

  and in `/etc/pf.conf` (replace `em0` with the uplink, or add just the `nat`
  line to an existing ruleset):

      ext_if = "em0"

      nat on $ext_if from 10.99.0.0/24 to any -> ($ext_if)

  `10.99.0.0/24` is used here; change it in the jail's `rc.conf` and the host
  pf rule if it collides with the host's LAN or another routed subnet.  Podman's
  own default network inside the jail (`10.88.0.0/16`) is in the jail's vnet and
  does not conflict with the host.

The storage dataset
-------------------

One-time setup on the host.  `-u` creates the dataset without mounting it, and
the mountpoint is the path the jail will see, not a host path:

    zfs create -u -o mountpoint=/var/db/containers zroot/podman
    zfs set jailed=on zroot/podman

The host does not mount it at any point: `jailed=on` makes the host's
`zfs mount -a` treat it as a jail dataset and skip it.  Inside the jail,
`zfs.dataset` in `podman.conf` attaches it, and `zfs_enable=YES` in the jail's
`rc.conf` mounts it at `/var/db/containers`.

The jail root
-------------

    mkdir -p /jail/podman
    bsdinstall jail /jail/podman

(or unpack `base.txz` from the distribution set into the same directory)

The jail's `/etc/rc.conf` supplies its side of the network and mounts its
storage dataset (`jail.rc.conf` in this directory):

    hostname="podman"
    zfs_enable="YES"
    pf_enable="YES"
    gateway_enable="YES"
    ifconfig_epair0b="inet 10.99.0.2/24"
    defaultrouter="10.99.0.1"
    podman_enable="YES"

`ifconfig_epair0b` is applied by `netif` after jail(8) moved the interface into
the jail's vnet; `zfs` mounts the attached storage dataset (the jail has
`allow.mount`); `pf` loads `/etc/pf.conf`; `gateway_enable` (rc.d/routing) turns
on forwarding in the jail's vnet; `podman_enable` starts the containers that
have a `--restart=always` policy.  All of these rc scripts run in vnet jails,
and rc order puts netif, pf and zfs before the podman service (`REQUIRE: LOGIN`).

The jail also needs working name resolution:

    cp /etc/resolv.conf /jail/podman/etc/resolv.conf

Install the configuration before the first start, so that `pf_enable` finds
`/etc/pf.conf`:

    mkdir -p /etc/jail.conf.d
    install -m 644 examples/podman-jail/podman.conf /etc/jail.conf.d/podman.conf
    install -m 644 examples/podman-jail/jail.rc.conf /jail/podman/etc/rc.conf
    install -m 644 examples/podman-jail/pf.conf /jail/podman/etc/pf.conf
    mkdir -p /jail/podman/usr/local/etc/containers
    install -m 644 examples/podman-jail/containers.conf \
        examples/podman-jail/storage.conf /jail/podman/usr/local/etc/containers/

`/etc/jail.conf` has to include `/etc/jail.conf.d/*.conf` for this to be used;
when that include is missing, rc.d/jail falls back to
`/etc/jail.conf.d/podman.conf` for the jail directly (a legacy
`/etc/jail.podman.conf` would take precedence over both).

Start it and install podman inside:

    service jail start podman
    pkg -j podman install -y podman

Then replace the packaged runtime with the one from this tree:

    bazel build //ocijail:ocijail
    install -m 755 bazel-bin/ocijail/ocijail /jail/podman/usr/local/bin/ocijail

`pkg -j` needs the jail to be running; later starts come from
`jail_enable=YES`.  The dataset is deliberately mounted at
`/var/db/containers` rather than at the graphroot itself: the zfs driver looks
up the dataset of `dirname(graphroot)`.

Running containers
------------------

    jexec podman podman info                 # runtime, storage driver, network
    jexec podman podman run --rm ghcr.io/freebsd/freebsd-runtime:15.1 uname -a

Give long-lived containers `--restart=always` and `podman_enable=YES` in the
jail's `rc.conf` brings them back whenever the jail starts (and stops them when
it shuts down).

Port publishing works the same way as on the host (`podman run -p 8080:80`):
it needs the `cni-rdr/*` anchors from `pf.conf`, and outbound traffic needs the
`<cni-nat>` table and the jail's pf to be running.  The jail's pf rules live in
the jail's vnet only; the host's pf is untouched by podman.

If something does not work, `jail -v -c podman` prints every `exec.*` command
and its exit status.  The output of the jail's own rc is lost unless the jail
has `exec.consolelog` set (for example `/var/log/podman-jail.log`), so add that
when debugging, or run `jexec podman service <name> status`.  Typical failures:

- `pfctl: /dev/pf: No such file or directory` - ruleset 5 is not what this
  example expects (see Notes) or the pf module is not loaded on the host;
- podman fails to create a container with "operation not permitted" - the jail
  is missing one of the `allow.*` parameters above (`children.max` for the
  child jail, `allow.mount.*` for its mounts, `allow.raw_sockets`);
- the zfs driver says `no zfs dataset found` - the storage dataset is not
  mounted at `/var/db/containers` inside the jail (`zfs_enable` failed, or
  `jailed=on` is not set), or `enforce_statfs` is not 1;
- containers cannot reach the network - the jail's pf was not loaded, the
  `<cni-nat>` table is empty (ipMasq), or forwarding is off in the jail's vnet.

Why each parameter
------------------

- `vnet`, `vnet.interface` - podman annotates containers with
  `org.freebsd.jail.vnet=new`, and a jail can only give a child parameters it
  has itself; the interface is the jail's uplink;
- `mount.devfs`, `devfs_ruleset = 5` - `/dev/zfs` for the zfs storage driver and
  the `zfs` userland, `/dev/pf` for the CNI plugins and `pfctl`;
- `allow.mount` plus the four `allow.mount.*` - ocijail mounts devfs on `/dev`,
  fdescfs on `/dev/fd`, tmpfs on `/dev/shm` and nullfs for bind mounts;
- `allow.mount.zfs` - the zfs driver mounts the layer datasets it creates, and
  `zfs_enable` in the jail mounts the storage dataset;
- `allow.chflags`, `allow.raw_sockets` - ocijail sets both on every container;
  `allow.extattr` - images and storage may want extended attributes;
- `enforce_statfs = 1` - mounts inside the jail are only permitted below 2, and
  the zfs driver has to see the mount of the storage dataset;
- `sysvmsg`/`sysvsem`/`sysvshm = "new"` - own IPC namespaces instead of the
  host's; `containers.conf` passes the same to the containers, because a child
  jail without them has no System V IPC at all;
- `children.max = 100` - one child jail per container;
- `persist`, `exec.start`, `exec.stop` - the jail runs its own rc, which brings
  up its network, mounts the dataset and starts pf;
- `zfs.dataset` - attaches `zroot/podman` to the jail after it exists and
  before `exec.start`; it warns when `jailed=on` is missing on the dataset;
- `exec.prestart`, `exec.poststop` - create the epair and put one end on the
  host bridge, and remove both ends again when the jail goes away.

Stopping the jail
-----------------

A clean stop runs the jail's rc.shutdown: `podman` stops the containers with
`--restart=always`, then `zfs` unmounts the storage dataset and its currently
mounted children, and pf stops.  Containers without a restart policy are not rc
services, so stop those first, otherwise their mounts are busy and stay behind:

    jexec podman podman stop -a
    service jail stop podman

There is no per-jail mount namespace on FreeBSD: a dataset the zfs driver
mounted inside the jail is a mount in the host's mount table.  Leftover mounts
below the jail root can be removed afterwards (deepest first):

    mount -p | awk '$2 ~ "^/jail/podman/" { print $2 }' | tail -r | xargs umount -f

That includes the storage dataset (`/jail/podman/var/db/containers`) if the
jail was killed before rc.shutdown could unmount it.  Do that before starting
the jail again, otherwise its `zfs mount -a` reports the dataset as already
mounted (the mount is in the right place, so storage still works).

Notes
-----

- `devfs_ruleset = 5` is `devfsrules_jail_vnet` in FreeBSD 15.1:
  `devfsrules_jail` (basic and login devices, `zfs`, `fuse`) plus `pf`.  If
  `/etc/defaults/devfs.rules` on your system differs, check
  `devfs rule showsets` and adjust the number or define a local ruleset;
- the epair name (`epair0`) is host-global; use a different pair for a second
  podman jail, or the two cannot start at the same time;
- ruleset 5 has no `bpf*`; that is only needed if a CNI IPAM plugin uses DHCP.
  Extend it with a local ruleset that includes `$devfsrules_jail_vnet` and adds
  `add path 'bpf*' unhide`;
- containers that run bhyve need `/dev/vmm`, `/dev/vmm.io` and `/dev/vmmctl`
  exposed by a ruleset as well, plus `allow.vmm` on the jail and on the
  container - the same pattern as in `examples/nested-podman`;
- a container root can reach the jail's pf (its own vnet) and `/dev/zfs`
  (datasets exported to this jail only); if that is too much, run a separate
  ruleset without them for the containers, but ocijail cannot apply one inside
  a jail, so that means changing the jail's own ruleset;
- a host resolver on `127.0.0.1` does not work in a vnet jail, point the jail's
  `/etc/resolv.conf` at a reachable address instead;
- podman inside the jail shells out to `zfs`, `ifconfig` and `sysctl` (storage
  driver, CNI plugins), so its `PATH` needs `/sbin`; this is the case for an
  interactive `jexec`, but check it if the jail is driven by a script;
- `exec.timeout = 30` applies to every `exec.*` command, including
  `/bin/sh /etc/rc`; raise it if services in the jail take longer to start;
- one level of nesting: the containers inside this jail get
  `children.max = 0`, so they cannot create jails themselves.
