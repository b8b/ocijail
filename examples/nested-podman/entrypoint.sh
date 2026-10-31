#!/bin/sh
#
# Prepare the container's vnet so the CNI plugins used by the podman inside it
# can NAT container traffic. pf is per-vnet: enabling it here only affects this
# container's network stack, the module itself has to be loaded on the host.
set -eu

egress="${EGRESS_IFACE:-}"
if [ -z "$egress" ]; then
    egress="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')"
fi
: "${egress:=eth0}"

cat > /etc/pf.conf <<EOF
# Written by /usr/local/bin/entrypoint.sh
set skip on lo0

table <cni-nat>
nat on $egress inet from <cni-nat> to any -> ($egress)
nat on $egress inet6 from <cni-nat> to !ff00::/8 -> ($egress)

rdr-anchor "cni-rdr/*"
nat-anchor "cni-rdr/*"
EOF

# The bridge plugin turns this on as well; doing it early makes the first
# container work even if a plugin ordering detail changes.
sysctl -q net.inet.ip.forwarding=1 >/dev/null 2>&1 || true
sysctl -q net.inet6.ip6.forwarding=1 >/dev/null 2>&1 || true

if [ ! -c /dev/pf ]; then
    echo "entrypoint: /dev/pf is missing; the devfs ruleset of the jail that" \
         "runs this container has to expose it" >&2
elif ! pfctl -f /etc/pf.conf; then
    echo "entrypoint: failed to load /etc/pf.conf" >&2
else
    pfctl -e >/dev/null 2>&1 || true
fi

exec "$@"
