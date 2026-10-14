# Xn handover bring-up: 1 UE, 3 gNBs

Minimal topology for debugging the Xn interface end to end before wiring the
dispatcher back in.

| node | RLS / N2 / N3 | Xn listen      | NCI           | nr-cli node name        |
|------|---------------|----------------|---------------|-------------------------|
| gnb1 | 127.0.0.1     | 127.0.0.1:38422 | `0x000000010` | `UERANSIM-gnb-999-70-1` |
| gnb2 | 127.0.0.2     | 127.0.0.2:38422 | `0x000000020` | `UERANSIM-gnb-999-70-2` |
| gnb3 | 127.0.0.3     | 127.0.0.3:38422 | `0x000000030` | `UERANSIM-gnb-999-70-3` |
| ue1  | search list = all three | — | supi `imsi-999700000000001` | `imsi-999700000000001` |

All three gNBs register to the same AMF (`127.0.0.5:38412`) with the same TAC
and the same S-NSSAI (`sst: 1`). The dispatcher is **commented out** in all
three gNB configs so the first run exercises Xn + NGAP PathSwitch on its own.

---

## 0. One-time setup

```bash
cd ~/5g-lab/UERANSIM
make                                   # wipes build/ and rebuilds into build/
```

Make sure `/etc/open5gs/amf.yaml` agrees with `config/xn3/open5gs-amf.yaml`
(PLMN 999/70, TAC 1, SST 1, NGAP bound to 127.0.0.5), then provision the
subscriber once:

```bash
open5gs-dbctl add 999700000000001 \
    465B5CE8B199B49FAA5F0A2EE238A6BC \
    E8ED289DEBA952E4283B54E88E6183CA
```

Give the latency CSV a header so the columns are readable while debugging. It
is written by the *target* gNB now, from `src/gnb/ngap/pathswitch.cpp`. Start a
fresh file: the columns changed, so older rows are not comparable.

```bash
echo "sst,ue_arrival_ms,psw_ms,total_ms" > /home/amirndr/5g-lab/xn_ho_latency.csv
```

## 0b. Data plane: run the UE in its own network namespace

**Required if you want to measure anything on the user plane** (packet loss
during a handover, throughput, ping continuity). Skip it only if you care solely
about control-plane signalling.

With everything in one namespace the UE's `10.45.0.x` is a *local* address of the
host, so the kernel's `local` routing table short-circuits every downlink packet
to a local socket. It never reaches `ogstun` -> UPF -> GTP -> gNB. The 5G
downlink is bypassed entirely, `ping -I uesimtun0` returns 100% loss, and no
data-plane measurement is meaningful. Moving the UE into its own namespace makes
`10.45.0.x` non-local, forcing downlink through the real path.

```bash
sudo ./config/xn3/setup-netns.sh          # idempotent; 'clean' tears down
```

Then use the `-netns` config variants. Only `linkIp` differs (the radio link must
be reachable from the namespace); `ngapIp`, `gtpIp` and `xnAddress` stay on
`127.0.0.x` because the gNBs and the core are both still in the main namespace.

```bash
sudo ./build/nr-gnb -c config/xn3/gnb1-netns.yaml
sudo ip netns exec ue1 ./build/nr-ue -c config/xn3/ue1-netns.yaml
sudo ./build/nr-gnb -c config/xn3/gnb2-netns.yaml

sudo ip netns exec ue1 ping -I uesimtun0 10.45.0.1
```

Two traps:

- **Always pass `-I uesimtun0`.** From inside the namespace, `ping 10.45.0.1`
  without it goes over the veth to the host and succeeds while proving nothing
  about the 5G path.
- **No `nr-ue` may be running in the main namespace.** It would create a
  `uesimtun0` there and reintroduce the exact local-address problem. The setup
  script refuses to run if it finds one.

`nr-cli` still works normally from the main namespace — the gNBs never moved.

## 1. Start the core

```bash
sudo systemctl restart open5gs-nrfd open5gs-scpd open5gs-amfd \
    open5gs-smfd open5gs-upfd open5gs-ausfd open5gs-udmd \
    open5gs-udrd open5gs-pcfd open5gs-bsfd

sudo journalctl -fu open5gs-amfd        # keep this open — it is your ground truth
```

## 2. Start the gNBs — order matters

Start **gnb1 alone first**, attach the UE, and only then bring up gnb2 and gnb3.
Every cell reports `dbm = -1` (see the note at the top of `ue1.yaml`), so if all
three are up before the UE starts, which cell it camps on is not deterministic.

**Terminal 1 — gnb1**
```bash
cd ~/5g-lab/UERANSIM
sudo ./build/nr-gnb -c config/xn3/gnb1.yaml
```
Wait for `NG Setup procedure is successful`.

**Terminal 2 — ue1**
```bash
cd ~/5g-lab/UERANSIM
sudo ./build/nr-ue -c config/xn3/ue1.yaml
```
Wait for `Connection setup for PDU session[1] is successful, TUN interface[uesimtun0, 10.45.0.x]`.

**Terminal 3 — gnb2**
```bash
cd ~/5g-lab/UERANSIM
sudo ./build/nr-gnb -c config/xn3/gnb2.yaml
```

**Terminal 4 — gnb3**
```bash
cd ~/5g-lab/UERANSIM
sudo ./build/nr-gnb -c config/xn3/gnb3.yaml
```

In each gNB log, confirm the Xn listener came up:
```
[xn] [info] Xn server listening on 127.0.0.2:38422
```
and cross-check the sockets:
```bash
ss -lntp | grep 38422        # expect three LISTEN rows, one per 127.0.0.x
```

## 3. Verify the pre-handover state

**Terminal 5 — control**
```bash
cd ~/5g-lab/UERANSIM
./build/nr-cli --dump                                        # all five nodes
./build/nr-cli UERANSIM-gnb-999-70-1 --exec "ue-list"        # UE is on gnb1
./build/nr-cli UERANSIM-gnb-999-70-2 --exec "ue-count"       # -> 0
./build/nr-cli UERANSIM-gnb-999-70-3 --exec "ue-count"       # -> 0
```

`ue-list` prints the gNB-local UE id you need next:
```yaml
- ue-id: 1
  ran-ngap-id: 1
  amf-ngap-id: 1
```

Optional traffic check (leave running in a sixth terminal to watch what the
handover does to the data plane):
```bash
ping -I uesimtun0 8.8.8.8
```

## 4. Trigger the handover

```bash
./build/nr-cli UERANSIM-gnb-999-70-1 --exec "xn-handover 1 127.0.0.2 38422 1"
#                                                        ^  ^          ^     ^
#                                                    ue-id  target-ip  port  sst
```
Port and SST default to `38422` and `1`, so `xn-handover 1 127.0.0.2` is
equivalent.

### What each log should show

Every line of the handover is tagged `[xn-ho]`, so `grep xn-ho` on any of the
three terminals gives you the whole story.

**gnb1 (source)**
```
[ngap] [info] [xn-ho] SOURCE: handover requested ue=1 target=127.0.0.2:38422 sti=0x...
[ngap] [info] [xn-ho] SOURCE: XnHandoverAck ok in 1ms, target nci=0x20
[ngap] [info] [xn-ho] SOURCE: context released locally (no RRC Release sent) | ue=1 sst=1
[rrc]  [info] [xn-ho] SOURCE: RRCReconfiguration(reconfigurationWithSync) -> UE[1], target nci=0x20
```

**ue1**
```
[rrc] [info] [xn-ho] UE: handover command received, target nci=0x20
[rrc] [info] [xn-ho] UE: serving cell 1 -> 2, staying RRC-CONNECTED
[rrc] [info] [xn-ho] UE: RRCReconfigurationComplete -> cell[2]
```

**gnb2 (target)**
```
[xn]   [info] [xn-ho] TARGET: XnHandoverRequest from 127.0.0.1 | sti=0x... sst=1 sessions=1
[xn]   [info] [xn-ho] TARGET: XnHandoverAck sent, awaiting UE arrival | nci=0x20
[rrc]  [info] [xn-ho] TARGET: UE arrived, ueId=1 sti=0x..., creating RRC context
[ngap] [info] [xn-ho] TARGET: PathSwitchRequest -> AMF | ueId=1 srcAmfId=1 ranUeId=1 sessions=1
[ngap] [debug] PathSwitchRequestAcknowledge received
[ngap] [info]  PDU session resource(s) setup for UE[1] count[1]
[ngap] [info] [xn-ho] TARGET: COMPLETE ueId=1 sst=1 sessions=1 | arrival=..ms psw=..ms total=..ms
```

Note the ordering: the path switch happens **after** the UE arrives, and the
NGAP context id equals the UE's real RLS/RRC id (`ueId=1`), not a synthetic one.

**AMF journal**
```
[amf] INFO: [imsi-999700000000001] Path Switch Request
[amf] INFO: [imsi-999700000000001] Path Switch Acknowledge
```

**CSV** — written by the *target*, columns `sst,ue_arrival_ms,psw_ms,total_ms`
where t0 = XnHandoverRequest received, t1 = UE completed its move, t2 = AMF
acknowledged the switch.
```bash
tail -f /home/amirndr/5g-lab/xn_ho_latency.csv
```

## 5. Chain a second handover

The UE keeps the same ue-id on gnb2 that it had on gnb1 (it is the RLS id, and
the RLS session predates the handover), so no lookup is needed:

```bash
./build/nr-cli UERANSIM-gnb-999-70-2 --exec "ue-list"
./build/nr-cli UERANSIM-gnb-999-70-2 --exec "xn-handover <id> 127.0.0.3"
```

---

## What "working" looks like

The handover is real connected-mode mobility: the UE is told to move by an
`RRCReconfiguration` carrying `reconfigurationWithSync` and switches cells while
staying `RRC-CONNECTED`. Check all of these:

1. **The UE never goes idle.** Its log must NOT contain `RRC Release received`,
   `CM-IDLE`, `MM-DEREGISTERED`, `Sending Initial Registration`, or
   `Sending Service Request` anywhere around the handover. Apart from the three
   `[xn-ho] UE:` lines, NAS should be completely silent — an intra-AMF,
   intra-TA handover requires no NAS signalling at all.

2. **One context on the target, not two.** `ue-count` on gnb2 must read `1`
   immediately and stay there. Two contexts, or a `UE Context Release Command`
   arriving at gnb2 shortly after the handover, means the UE cold-reattached
   instead of being handed over.

3. **The AMF sees only a path switch.** No `Initial Context Setup` or
   registration for this UE in the AMF journal around the handover.

4. **The data plane barely notices.** Run `ping -I uesimtun0 8.8.8.8` across the
   handover: expect one or two lost packets while the path switch completes, and
   the same TUN address afterwards. There is still no data forwarding and no SN
   Status Transfer, so the switch is not lossless by design.

## Design notes worth knowing

- **The target cell is identified by NCI, carried in `physCellId`.** 3GPP has no
  field in `reconfigurationWithSync` wide enough for a 36-bit NCI, and UERANSIM
  has no PCI concept, so the NCI travels in `physCellId` — which is constrained
  to `0..1007`. The xn3 NCIs (`0x10`/`0x20`/`0x30` = 16/32/48) fit. A larger NCI
  is rejected with a clear error rather than silently truncated.

- **The UE already knows every cell before the handover.** It heartbeats every
  gNB in `gnbSearchList` continuously and learns each cell's NCI from SIB1, so
  resolving the target is a local lookup. This is also why the target already
  has an RLS `ueId` for the UE before the handover starts — that id is what the
  whole procedure is keyed on, correlated across gNBs by the UE's STI.

- **gnb1 does not wait for the UE to arrive.** The CLI handler pauses gnb1's RRC
  and RLS tasks, so blocking until the target confirms would deadlock: the
  handover command itself could never be delivered. gnb1 sends the command,
  releases its context, and returns; the target owns the rest.

## Useful failure injections

| test | command | expected |
|------|---------|----------|
| target down | kill gnb3, then `xn-handover <id> 127.0.0.3` | `[xn-ho] SOURCE: preparation failed — target gNB unreachable`; UE stays put, no CSV row |
| target up, AMF down | `sudo systemctl stop open5gs-amfd`, trigger HO | UE still moves and `TARGET: UE arrived` appears, but no `COMPLETE` line and no CSV row — the path switch never completes |
| NCI out of range | set a target `nci` above `0x3EF` (1007) and retry | `[xn-ho] SOURCE: target nci ... exceeds physCellId range`, handover aborted before the UE is touched |
