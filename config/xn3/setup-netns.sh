#!/bin/bash
# ── UE network namespace setup for the Xn 3-gNB topology ─────────────────────
#
# WHY THIS EXISTS
#
# With everything in one network namespace, the UE's TUN address (10.45.0.x) is
# a *local* address of the host. The kernel's `local` routing table is consulted
# before all others, so any downlink packet for the UE is delivered straight to
# a local socket and never goes out ogstun -> UPF -> GTP -> gNB. The 5G downlink
# path is silently bypassed, `ping -I uesimtun0` fails, and no data-plane
# measurement of a handover is possible.
#
# Running the UE in its own namespace makes 10.45.0.x non-local to the host, so
# downlink has to traverse the real path.
#
# The UE reaches the gNBs over RLS (UDP 4997). Loopback is per-namespace, so the
# gNBs cannot stay on 127.0.0.x for the *radio* link -- they move to a veth
# network. Only linkIp changes; ngapIp/gtpIp stay on 127.0.0.x because the gNBs
# and the core are both still in the main namespace.
#
#   main namespace                         ue1 namespace
#   ------------------------------------   -----------------
#   gnb1 linkIp 10.200.0.11  \             10.200.0.2
#   gnb2 linkIp 10.200.0.12   >- veth ---- veth-ue1-ns
#   gnb3 linkIp 10.200.0.13  /             uesimtun0 10.45.0.x
#   ogstun 10.45.0.1 (UPF)
#
# Usage:  sudo ./config/xn3/setup-netns.sh          (idempotent)
#         sudo ./config/xn3/setup-netns.sh clean    (tear down)

set -e

NS=ue1
VETH_HOST=veth-ue1
VETH_NS=veth-ue1-ns
NS_ADDR=10.200.0.2/24
GNB_ADDRS=(10.200.0.11 10.200.0.12 10.200.0.13)

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0 $*" >&2
    exit 1
fi

cleanup() {
    echo "Tearing down..."
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$VETH_HOST" 2>/dev/null || true

    # Rules and interfaces left behind by a UE that ran in the main namespace.
    # These are what make the host treat 10.45.0.x as local.
    while ip rule list | grep -q rt_uesimtun0; do
        ip rule del $(ip rule list | grep rt_uesimtun0 | head -1 | \
                      sed 's/^[0-9]*:[[:space:]]*//') 2>/dev/null || break
    done
    for i in $(seq 0 9); do
        ip link del "uesimtun$i" 2>/dev/null || true
    done
    echo "Done."
}

if [[ "$1" == "clean" ]]; then
    cleanup
    exit 0
fi

if pgrep -f "nr-ue" >/dev/null; then
    echo "ERROR: an nr-ue is still running. Kill it first -- if it is running in" >&2
    echo "       the main namespace it holds a uesimtun0 there, which reintroduces" >&2
    echo "       the exact local-address problem this script exists to avoid." >&2
    echo "       sudo pkill -f nr-ue" >&2
    exit 1
fi

# Start from a clean slate so re-running is safe.
cleanup >/dev/null 2>&1 || true

echo "Creating namespace '$NS' and veth pair..."
ip netns add "$NS"
ip link add "$VETH_HOST" type veth peer name "$VETH_NS"
ip link set "$VETH_NS" netns "$NS"

# One host address per gNB: each binds its RLS socket to a distinct linkIp.
for addr in "${GNB_ADDRS[@]}"; do
    ip addr add "$addr/24" dev "$VETH_HOST"
done
ip link set "$VETH_HOST" up

ip netns exec "$NS" ip link set lo up
ip netns exec "$NS" ip addr add "$NS_ADDR" dev "$VETH_NS"
ip netns exec "$NS" ip link set "$VETH_NS" up
ip netns exec "$NS" ip route add default via "${GNB_ADDRS[0]}"

echo
echo "Ready. The gNBs stay in the main namespace; only the UE moves:"
echo
echo "  sudo ./build/nr-gnb -c config/xn3/gnb1-netns.yaml"
echo "  sudo ./build/nr-gnb -c config/xn3/gnb2-netns.yaml"
echo "  sudo ip netns exec $NS ./build/nr-ue -c config/xn3/ue1-netns.yaml"
echo
echo "Then, inside the namespace, this now really traverses GTP:"
echo "  sudo ip netns exec $NS ping -I uesimtun0 10.45.0.1"
echo
echo "NOTE: pinging 10.45.0.1 WITHOUT -I uesimtun0 goes over the veth and proves"
echo "      nothing about the 5G path. Always pass -I uesimtun0."
