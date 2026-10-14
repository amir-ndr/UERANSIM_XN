# Xn handover in this UERANSIM fork

This repository is **UERANSIM by Ali Güngör** (<https://github.com/aligungr/UERANSIM>)
extended with **Xn-based, connected-mode handover** between gNBs that share one
AMF. The Xn handover is new; everything else is upstream UERANSIM.

**Base:** upstream `master` at commit
[`b4157fa`](https://github.com/aligungr/UERANSIM/commit/b4157fa) (25 Oct 2025).
That commit still reports itself as **v3.2.7**. It is the v3.2.7 release plus
upstream's later commits: the AKMA key derivation, the `<cstdint>` build fixes,
and the move to the AGPL-3.0 license.

This document explains what was added, first in concept and then file by file in
the source code. To see the exact changes yourself, compare this repository with
the base commit:

```bash
git clone https://github.com/aligungr/UERANSIM.git ueransim-upstream
git -C ueransim-upstream checkout b4157fa
diff -rq ueransim-upstream/src <this-repository>/src
```

For a ready-to-run example, start with the [Quick start](#0-quick-start-one-ue-two-gnbs-one-handover)
below. A larger three-gNB debug setup (with user-plane testing in network
namespaces) is described in [`config/xn3/README.md`](config/xn3/README.md).

---

## 0. Quick start: one UE, two gNBs, one handover

Everything runs on one Ubuntu machine, on loopback addresses (`127.0.0.x` works
without any network setup). The example configs are in
[`config/xn-handover-example/`](config/xn-handover-example/):

| File | Role | RLS / N2 / N3 IP | Xn listen | NCI | `nr-cli` name |
|---|---|---|---|---|---|
| `gnb1.yaml` | source cell | 127.0.0.1 | 127.0.0.1:38422 | `0x10` (16) | `UERANSIM-gnb-999-70-1` |
| `gnb2.yaml` | target cell | 127.0.0.2 | 127.0.0.2:38422 | `0x20` (32) | `UERANSIM-gnb-999-70-2` |
| `ue1.yaml` | the UE, hears both cells | `gnbSearchList`: 127.0.0.1, 127.0.0.2 | — | — | `imsi-999700000000001` |

All of them use PLMN **999/70**, TAC **1**, slice **SST 1** and the AMF at
**127.0.0.5:38412**, which are the Open5GS defaults.

### Step 1: Open5GS (the 5G core)

Open5GS is **not** part of this repository: clone it and build it yourself.
Check the official guide for the current dependency list:
<https://open5gs.org/open5gs/docs/guide/02-building-open5gs-from-sources/>.

```bash
# 1a. MongoDB (Open5GS keeps subscribers in it): install it following
#     https://www.mongodb.com/docs/manual/installation/ , then start it:
sudo systemctl start mongod

# 1b. Build dependencies, clone and build
sudo apt install -y python3-pip python3-setuptools python3-wheel ninja-build \
    build-essential flex bison git cmake meson libsctp-dev libgnutls28-dev \
    libgcrypt-dev libssl-dev libmongoc-dev libbson-dev libyaml-dev libnghttp2-dev \
    libmicrohttpd-dev libcurl4-gnutls-dev libtins-dev libtalloc-dev
git clone https://github.com/open5gs/open5gs
cd open5gs
meson build --prefix=`pwd`/install
ninja -C build
cd build && ninja install && cd ..

# 1c. TUN device for the UPF (user plane)
sudo ip tuntap add name ogstun mode tun
sudo ip addr add 10.45.0.1/16 dev ogstun
sudo ip link set ogstun up
```

> **No build needed?** On Ubuntu you can install the packaged version instead:
> `sudo add-apt-repository ppa:open5gs/latest && sudo apt update && sudo apt install open5gs`.
> The configs are then in `/etc/open5gs/`, the network functions run as systemd
> services, and `ogstun` is created for you.

**Check the AMF config** (`install/etc/open5gs/amf.yaml`, or
`/etc/open5gs/amf.yaml` for the package). It must have the NGAP server on
`127.0.0.5`, PLMN `999`/`70`, `tac: 1` and `s_nssai` with `sst: 1`. These are the
defaults; [`config/xn3/open5gs-amf.yaml`](config/xn3/open5gs-amf.yaml) shows a
complete working `amf:` section.

**Start the core** (source build; the NRF and SCP first):

```bash
cd install/bin
for nf in nrfd scpd; do nohup ./open5gs-$nf > /tmp/open5gs-$nf.out 2>&1 & done
sleep 2
for nf in amfd smfd ausfd udmd udrd pcfd nssfd bsfd; do
    nohup ./open5gs-$nf > /tmp/open5gs-$nf.out 2>&1 &
done
sudo -v && sudo nohup ./open5gs-upfd > /tmp/open5gs-upfd.out 2>&1 &
cd ../..
```

(Package install: `sudo systemctl restart open5gs-*` instead.)

**Add the example subscriber** (IMSI, key and OPc from `ue1.yaml`), from the
Open5GS source tree:

```bash
./misc/db/open5gs-dbctl add 999700000000001 \
    465B5CE8B199B49FAA5F0A2EE238A6BC E8ED289DEBA952E4283B54E88E6183CA
```

### Step 2: build this UERANSIM

```bash
sudo apt install -y make gcc g++ libsctp-dev lksctp-tools iproute2
sudo snap install cmake --classic          # CMake >= 3.17
cd UERANSIM                                # this folder
make                                       # builds into build/
ls build/                                  # nr-gnb  nr-ue  nr-cli  ...
```

### Step 3: start gnb1, the UE, then gnb2 (in this order)

Use three terminals, all inside the `UERANSIM` folder. **The order matters.**
Every simulated cell has the same signal strength, so a UE started with both
cells up picks one at random. Starting gnb2 *after* the UE has registered on gnb1
makes gnb1 the source. The UE still hears gnb2 (it keeps heartbeating every
address in `gnbSearchList`) and learns gnb2's NCI, which the handover needs.

```bash
# Terminal 1 -- source gNB. Wait for:  "NG Setup procedure is successful"
#                                 and: "Xn server listening on 127.0.0.1:38422"
./build/nr-gnb -c config/xn-handover-example/gnb1.yaml

# Terminal 2 -- the UE (sudo: it creates the uesimtun0 interface). Wait for:
#   "Initial Registration is successful"
#   "PDU Session establishment is successful"
sudo ./build/nr-ue -c config/xn-handover-example/ue1.yaml

# Terminal 3 -- target gNB. Wait for: "NG Setup procedure is successful"
#                                and: "Xn server listening on 127.0.0.2:38422"
./build/nr-gnb -c config/xn-handover-example/gnb2.yaml
```

### Step 4: hand the UE over from gnb1 to gnb2

In a fourth terminal:

```bash
./build/nr-cli --dump                              # lists the two gNBs and the UE
./build/nr-cli UERANSIM-gnb-999-70-1 -e ue-list    # note the UE's ue-id on gnb1, e.g. 1
./build/nr-cli UERANSIM-gnb-999-70-1 -e "xn-handover 1 127.0.0.2"
# -> "Xn handover triggered"
```

The general form is `xn-handover <ue-id> <target-xn-ip> [target-xn-port] [slice-sst]`.
`<ue-id>` is the UE's id **on the source gNB**, and `<target-xn-ip>` is the
target's `xnAddress`. The port defaults to 38422 and the SST to 1.

### Step 5: check that it worked

```bash
./build/nr-cli UERANSIM-gnb-999-70-1 -e ue-list    # empty: the UE has left gnb1
./build/nr-cli UERANSIM-gnb-999-70-2 -e ue-list    # the UE, with a NEW ue-id but the SAME amf-ngap-id
./build/nr-cli imsi-999700000000001 -e status      # still registered and CM-CONNECTED; current-cell changed
```

The logs show every step, tagged `[xn-ho]`:

| Where | Lines, in order |
|---|---|
| gnb1 (source) | `SOURCE: handover requested` → `SOURCE: XnHandoverAck ok` → `SOURCE: RRCReconfiguration(reconfigurationWithSync) -> UE[1]` → `SOURCE: context released locally` |
| UE | `UE: handover command received, target nci=0x20` → `UE: serving cell … -> …, staying RRC-CONNECTED` → `UE: RRCReconfigurationComplete -> cell[…]` |
| gnb2 (target) | `TARGET: XnHandoverRequest from 127.0.0.1` → `TARGET: XnHandoverAck sent` → `TARGET: UE arrived` → `TARGET: PathSwitchRequest -> AMF` → `TARGET: COMPLETE … arrival=…ms psw=…ms total=…ms` |

A successful handover does **not** produce a new "Registration" in the UE or
AMF logs: the UE stays registered and keeps its PDU session and IP address.

**Hand it back** to gnb1 the same way, from gnb2, using the UE's id on gnb2:

```bash
./build/nr-cli UERANSIM-gnb-999-70-2 -e ue-list    # e.g. ue-id 1
./build/nr-cli UERANSIM-gnb-999-70-2 -e "xn-handover 1 127.0.0.1"
```

### Notes

- **Latency CSV.** The target appends one row per handover to
  `/home/amirndr/5g-lab/xn_ho_latency.csv`. This path is hard-coded in
  `src/gnb/ngap/pathswitch.cpp`; on another machine the row is silently skipped
  unless you change the path there and run `make` again (columns: section 5).
- **User-plane test** (ping during a handover). With the UE in the same network
  namespace as the core, downlink packets bypass the 5G path. Use the namespace
  setup in [`config/xn3/README.md`](config/xn3/README.md) (section 0b) for
  data-plane measurements.
- **More cells.** Add a gNB by copying `gnb2.yaml` with a new `nci` (≤ 1007 if it
  can be a handover target), new IPs, and its `linkIp` in the UE's
  `gnbSearchList`.

---

## 1. What upstream UERANSIM cannot do

In upstream UERANSIM a UE can only change cell the idle-mode way: it loses the
cell, re-selects another one and **registers again** with the core. There is no
handover of a connected UE. This holds for the base commit `b4157fa`, and still
for upstream `master` as of October 2026 (v3.3.0). In the source code:

| What a handover needs | Upstream UERANSIM |
|---|---|
| An Xn interface between gNBs | Does not exist. A gNB only talks to the UE (RLS), the AMF (N2/NGAP) and the UPF (N3/GTP). |
| gNB receives `RRCReconfigurationComplete` | `src/gnb/rrc/channel.cpp`: `case ...rrcReconfigurationComplete: break; // TODO` |
| gNB receives `MeasurementReport` | Same file: `break; // TODO` |
| UE handles `RRCReconfiguration` (the handover command) | Not handled; `src/ue/rrc/channel.cpp` drops it. |
| NGAP Path Switch Request / Acknowledge | Only listed as a procedure code in `transport.cpp`; no code builds the request or handles the acknowledge. |

## 2. The idea (intuitive)

A standard Xn handover (3GPP TS 38.300 §9.2.3.2) has three phases. The source gNB
is the cell the UE is leaving, the target gNB is the cell it moves to.

1. **Preparation (gNB ↔ gNB over Xn).** The source asks the target to accept the
   UE and sends it the UE's context: who the UE is to the AMF, and its PDU
   sessions (the GTP tunnels and QoS flows). The target agrees.
2. **Execution (gNB → UE over the radio).** The source sends the UE an
   `RRCReconfiguration` with `reconfigurationWithSync`, which says "move to cell X".
   The UE moves **without leaving RRC-CONNECTED**, so NAS never notices, and
   reports `RRCReconfigurationComplete` to the target.
3. **Completion (target gNB ↔ core).** The target tells the AMF "this UE is now
   with me, send its downlink here" (NGAP **Path Switch Request**, TS 38.413).
   The AMF and SMF move the UPF's downlink tunnel to the target and answer with
   **Path Switch Request Acknowledge**. The source's context is released.

The point of the handover is what does **not** happen: no new registration, no
new PDU session, no new IP address. The UE keeps its AMF context (its AMF UE NGAP
ID survives the move) and its sessions; only the radio cell and the downlink
tunnel change.

This fork implements those three phases, with a few simplifications (section 6).

### End-to-end flow in this fork

```
 harness / user          SOURCE gNB                    TARGET gNB                   UE            AMF/SMF/UPF
 ──────────────          ──────────                    ──────────                   ──            ───────────
 nr-cli xn-handover ───► CLI handler
                         └─► NGAP task: triggerXnHandover
                             reads UE's sessions + RLS STI
                             XnHandoverRequest (TCP/JSON) ───► XnTask
                                                              stores pending[STI]
                             ◄─────────────── XnHandoverAck {targetNci}
                         RRC: RRCReconfiguration ───────────────────────────────────► UE RRC
                              (reconfigurationWithSync,                               finds target cell
                               physCellId = target NCI)                               by NCI (SIB1),
                         drops RRC + NGAP + GTP context                               switches serving
                         (no RRC Release)                                             cell, stays CONNECTED
                                                              RRC ◄──────── RRCReconfigurationComplete
                                                              takePending(STI)
                                                              NGAP: PathSwitchRequest ─────────────────────► AMF
                                                                (source AMF-UE-NGAP-ID,                      SMF/UPF move
                                                                 new DL TEID per session)                    the DL tunnel
                                                              ◄──────────────── PathSwitchRequestAcknowledge
                                                              GTP: re-create sessions
                                                              writes latency row (CSV)
```

## 3. Design decisions and why

| Decision | Reason |
|---|---|
| **The UE is identified across gNBs by its RLS STI.** | UERANSIM's simulated radio link (RLS) gives every UE a random 64-bit STI that it sends in its heartbeats to *every* gNB in its `gnbSearchList`. It is the only identity both gNBs already know. The target stores the pending handover under the STI, and recognises the UE when its `RRCReconfigurationComplete` arrives on a link with that STI. |
| **The target cell travels as NCI inside `physCellId`.** | UERANSIM has no Physical Cell ID. The UE already knows each gNB's NCI from its SIB1, so the NCI is put in the 3GPP PCI field. Because `physCellId` is limited to 0…1007, target NCIs must be ≤ 1007 (checked in `rrc/handover.cpp`). |
| **Path switch is sent only after the UE has arrived.** | Sending it on the Xn request (an earlier version did this) would move the downlink to a target the UE may never reach. The target keeps the context pending until `RRCReconfigurationComplete`. |
| **The source drops its context right after the handover command, without an RRC Release.** | An RRC Release would push the UE to idle and trigger re-registration. The source also cannot wait for the target: the command must be delivered first. |
| **NAS is not told the cell changed.** | The UE's RRC switches the serving cell itself and does *not* send `ACTIVE_CELL_CHANGED` to NAS; otherwise NAS would start a registration update and defeat the handover. |
| **The handover runs on the NGAP task, not inside the CLI handler.** | The CLI handler pauses the RRC, RLS and GTP tasks, and a paused task resumes only on a 20 ms polling boundary. Doing the work inline would add about 10 ms of simulator-only delay to every measured handover. |
| **The gNB keeps a UE's RLS id if the UE briefly loses signal.** | Upstream erases the STI→ueId mapping when heartbeats stop, so a returning UE gets a *new* id and its RRC/NGAP/GTP contexts (keyed by the old id) are orphaned. With UEs heartbeating several gNBs and moving between them, this happened often; the fork keeps the mapping. |
| **Xn is a small TCP + JSON protocol, not XnAP.** | Only the information the target needs is exchanged; full XnAP (TS 38.423, ASN.1 over SCTP) was not necessary for these experiments. |

## 4. Source changes, file by file

All paths are under `src/`. "New" files do not exist upstream. Line counts are
approximate.

### 4.1 Xn interface (new)

**`gnb/xn/task.hpp`, `gnb/xn/task.cpp`** (~470 lines): the `XnTask`, a TCP
server on `xnAddress:xnPort` (default port 38422) running in its own thread.

- **Wire format:** each message is a 4-byte big-endian length followed by a JSON
  object.
- **`XnHandoverRequest`** (source → target):

  ```json
  {"type":"XnHandoverRequest","ueId":7,"sti":1234567890123,
   "amfUeNgapId":42,"ranUeNgapId":7,"sliceSst":1,
   "sourceGnb":"127.0.0.1",
   "sessions":"1,305419896,7f000007,1"}
  ```

  `sessions` is `psi,upTeid,upAddrHex,qfi1+qfi2|psi2,...`: for each PDU session,
  the UPF's uplink tunnel (TEID and address) and its QoS flow identifiers.
- **On receipt:** the target rejects the request if the STI, the sessions or the
  AMF UE NGAP ID is missing. Otherwise it stores an `XnPendingHandover`
  `{amfUeNgapId, sessions, sliceSst, requestedUs}` under the STI and replies:

  ```json
  {"type":"XnHandoverAck","status":"OK","targetCell":"UERANSIM-gnb-999-70-2",
   "targetNci":32,"selectedAmf":"127.0.0.5","pswLatencyMs":0.0}
  ```

- **`takePendingHandover(sti)`:** called by the target's RRC when the UE arrives;
  returns and removes the pending context. The map is guarded by a mutex because
  the RRC task runs on another thread.
- **Optional dispatcher hook:** if `dispatcherAddress` is configured, the target
  first sends a `PathSwitchRequest` JSON to an external (Python) dispatcher and
  records its `selectedAmf` and round-trip time. This is informational; the
  handover always uses the AMF the gNB is already connected to.

### 4.2 NGAP (N2)

| File | Change |
|---|---|
| **`gnb/ngap/handover.cpp`** (new, ~260 lines) | `triggerXnHandover(ueId, targetIp, targetPort, sst)`, the **source** side. It reads the UE's STI from RLS and its stored session tunnels, sends `XnHandoverRequest`, waits for the ack (30 s timeout), then asks RRC to send the handover command and releases the UE's GTP and NGAP context locally. |
| **`gnb/ngap/pathswitch.cpp`** (new, ~290 lines) | The **target** side. `sendPathSwitchRequest` creates a new NGAP UE context for the arrived UE (new RAN UE NGAP ID, the *source's* AMF UE NGAP ID) and builds a real ASN.1 **PathSwitchRequest**: Source-AMF-UE-NGAP-ID; UESecurityCapabilities (fixed NEA1/2, NIA1/2); and, per session, a `PathSwitchRequestTransfer` with a newly allocated downlink TEID on this gNB and the accepted QFIs. `receivePathSwitchRequestAcknowledge` decodes each `PathSwitchRequestAcknowledgeTransfer` (the core may return a new uplink tunnel), registers the UE with GTP and re-creates the PDU sessions there. It then logs the timing and appends a row to `/home/amirndr/5g-lab/xn_ho_latency.csv`. |
| `gnb/ngap/session.cpp` | At PDU session setup, saves each session's uplink TEID/address, downlink TEID and QFIs in `NgapUeContext::xnSessionInfo`, so the source can send them to the target later. |
| `gnb/ngap/task.hpp/.cpp` | Declares the three functions above; routes two new inter-task messages (`GNB_XN_TO_NGAP` → path switch, `GNB_CLI_TO_NGAP` → trigger handover). |
| `gnb/ngap/transport.cpp` | Dispatches the `PathSwitchRequestAcknowledge` successful outcome (upstream logged it as unhandled). |
| `gnb/ngap/nnsf.cpp` | AMF selection skips AMFs that are not connected, and falls back to any connected AMF when no slice is requested or none matches (upstream returned no AMF, e.g. for a Service Request without NSSAI). Robustness change, not specific to Xn. |

### 4.3 RRC (radio control)

| File | Change |
|---|---|
| **`gnb/rrc/handover.cpp`** (new, ~150 lines) | **Source:** `sendXnHandoverCommand(ueId, targetNci)` builds an ASN.1 `RRCReconfiguration` whose `masterCellGroup` carries a `CellGroupConfig → SpCellConfig → ReconfigurationWithSync` with `physCellId = targetNci`, `newUE-Identity` and `t304 = 1000 ms`. It sends this on DL-DCCH and deletes the RRC context *without* an RRC Release. **Target:** `receiveRrcReconfigurationComplete` looks up the sender's STI, takes the pending handover from `XnTask`, creates the RRC context and asks NGAP to send the path switch. |
| `gnb/rrc/channel.cpp` | Routes `rrcReconfigurationComplete` to the new handler (was `// TODO`). |
| `gnb/rrc/task.hpp/.cpp` | Declarations; handles the new `XN_HANDOVER_COMMAND` message from NGAP. |
| **`ue/rrc/handover.cpp`** (new, ~140 lines) | **UE:** `receiveRrcReconfiguration` decodes the `CellGroupConfig` and reads the target NCI from `reconfigurationWithSync`. It finds the cell with that NCI among the cells it hears (from SIB1), makes it the serving cell (`ASSIGN_CURRENT_CELL` to RLS) and answers `RRCReconfigurationComplete` through the new cell. It stays RRC-CONNECTED and does not inform NAS. |
| `ue/rrc/channel.cpp`, `ue/rrc/task.hpp` | Routes `rrcReconfiguration` on DL-DCCH to the new handler. |

### 4.4 RLS (simulated radio link)

| File | Change |
|---|---|
| `gnb/rls/udp_task.hpp/.cpp` | Adds `stiOf(ueId)`, the UE's STI for a local ueId (the handover correlation key). Adds a mutex around the STI/UE maps, because NGAP and RRC now read them from other threads. **Keeps the STI→ueId mapping when a UE's heartbeat times out**, so a UE that comes back keeps its ueId and contexts (section 3). |
| `gnb/rls/task.hpp/.cpp` | Passes `stiOf()` through to the UDP task. |

### 4.5 Messages, configuration, CLI, utilities

| File | Change |
|---|---|
| `gnb/nts.hpp`, `utils/nts.hpp` | New inter-task messages: `XN_HANDOVER_COMMAND` (NGAP → RRC, carries the target NCI), `NmGnbXnToNgap` (RRC → NGAP path switch, with sessions and timestamps), `NmGnbCliToNgap` (CLI → NGAP trigger). |
| `gnb/types.hpp` | `NgapUeContext::PduSessionXnInfo` (per-session tunnels and QFIs) plus handover timestamps; new config fields `xnAddress`, `xnPort` (38422), `dispatcherAddress`, `dispatcherPort` (9000); `XnTask*` in `TaskBase`. |
| `gnb.cpp` | Reads the four new YAML keys (all optional). |
| `gnb/gnb.cpp` | Creates, starts and stops the `XnTask` with the other tasks. |
| `lib/app/cli_cmd.hpp/.cpp`, `gnb/app/cmd_handler.cpp` | New gNB CLI command `xn-handover <ue-id> <target-ip> [target-port] [slice-sst]`. The handler checks the UE exists and posts the work to the NGAP task. |
| `utils/common.hpp/.cpp` | `CurrentTimeMicros()`: handover phases are sub-millisecond to a few ms, so millisecond timestamps would round them to 0 or 1. |

### 4.6 What is *not* part of this fork

Compared with the **base commit `b4157fa`**, the files in sections 4.1–4.5 are
the only source changes. If you compare with the older **v3.2.7 tag** instead, you
will also see the AKMA key derivation (`ue/nas/keys.*`, `ue/nas/mm/auth.cpp`) and
`#include <cstdint>` build fixes. Those are upstream commits (`b5ec52e`,
`8ffce53`) that the base already contains; they are not part of the Xn work.

No build-file change is needed: `src/gnb/CMakeLists.txt` collects all `.cpp`
files recursively, so the new `xn/` directory is compiled automatically.

## 5. Using it

**gNB config**, added to the usual UERANSIM gNB YAML:

```yaml
xnAddress: 127.0.0.2      # this gNB's Xn listen IP (Xn is off if omitted)
xnPort: 38422             # optional, default 38422
# dispatcherAddress: 127.0.0.1   # optional external dispatcher hook
# dispatcherPort: 9000
```

All gNBs must use the same AMF and TAC. Each UE's `gnbSearchList` must include
both the source and target gNBs, so the UE already hears the target cell and
knows its NCI. Target NCIs must be ≤ 1007.

**Trigger a handover** of UE 3 (its id on the *source* gNB) to the gNB whose Xn
server is at 127.0.0.2:

```bash
./build/nr-cli UERANSIM-gnb-999-70-1 -e "xn-handover 3 127.0.0.2"
./build/nr-cli UERANSIM-gnb-999-70-1 -e "xn-handover 3 127.0.0.2 38422 1"   # explicit port and SST
```

**Outputs:**

- **Logs:** every step is tagged `[xn-ho]` with `SOURCE:`, `TARGET:` or `UE:`.
  The target ends with
  `[xn-ho] TARGET: COMPLETE ... arrival=…ms psw=…ms total=…ms`.
- **Latency CSV:** one row per handover in
  `/home/amirndr/5g-lab/xn_ho_latency.csv` (path hard-coded in `pathswitch.cpp`),
  with columns `sst, ue_arrival_ms, psw_ms, total_ms`:
  - `ue_arrival_ms`: Xn request received → UE's RRCReconfigurationComplete;
  - `psw_ms`: UE arrived → AMF acknowledged the path switch;
  - `total_ms`: the sum of the two.
- **Identity after the move:** the UE gets a **new local ue-id** on the target
  (ids are per gNB), but keeps its **amf-ngap-id**. `nr-cli <gnb> -e ue-list`
  shows both, which is how a test harness can find the UE again.

## 6. Simplifications and limitations

- **No measurement-based decision.** UEs send no measurement reports
  (`MeasurementReport` is still `// TODO`). The handover is triggered externally
  through the CLI; in the HOLD experiments, the driver decides when and where.
- **Xn is not standard XnAP.** It is TCP + JSON, carries only the fields above,
  and has no SCTP, no ASN.1, and no XnAP *UE Context Release* or *SN Status
  Transfer*.
- **No data forwarding.** Downlink packets that reach the source between the
  handover command and the path switch are not forwarded to the target.
- **Simplified security.** No K_gNB\* key refresh (TS 33.501), and the
  UESecurityCapabilities in the PathSwitchRequest are fixed rather than taken
  from the UE's context.
- **Same AMF and TA only.** No AMF relocation, no N2-based (via-AMF) handover,
  no inter-TAC mobility update.
- **Ignored RRC fields.** The UE does not run timer T304 and does not use the
  `newUE-Identity` (C-RNTI) it is given; the move cannot fail at the radio level
  once the target cell is in coverage.
- **NCI range.** Target NCIs are limited to 0…1007 because they travel in
  `physCellId`.
- **Hard-coded CSV path.** The latency CSV location is fixed in
  `src/gnb/ngap/pathswitch.cpp`.

## 7. Credit and license

- **UERANSIM** is by **Ali Güngör** and contributors:
  <https://github.com/aligungr/UERANSIM>. This repository is a modified version of
  it, based on upstream commit `b4157fa`. All UERANSIM functionality other than
  the Xn handover (UE and gNB simulation, NAS, RRC, NGAP, GTP, RLS, the ASN.1
  code) comes from upstream unchanged.
- The **Xn handover extension** described in sections 2–6 was added on top of that
  base in 2026.
- **License:** the same as upstream, **GNU AGPL-3.0** (see [`LICENSE`](LICENSE)).
  The modified files and the added files are listed in section 4, as the license
  requires for a modified version.
