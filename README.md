# UERANSIM_XN: UERANSIM with Xn handover

**UERANSIM_XN** is a modified version of [UERANSIM](https://github.com/aligungr/UERANSIM),
the open-source 5G UE and gNB simulator by Ali Güngör, that adds **Xn-based
handover**. A UE that is connected and has an active PDU session can be moved
from one gNB to another **without re-registering**. Its registration, its PDU
session and its IP address all stay the same; only the serving cell and the
downlink tunnel change.

Everything else in this repository is upstream UERANSIM. Its original README is
kept as [`README_UERANSIM.md`](README_UERANSIM.md).

- **Based on:** upstream commit [`b4157fa`](https://github.com/aligungr/UERANSIM/commit/b4157fa)
  (UERANSIM v3.2.7 plus later upstream fixes, October 2025).
- **Xn handover by:** Amir Naderi, 2026.
- **License:** GNU AGPL-3.0, the same as UERANSIM.

---

## Contents

1. [What you get](#1-what-you-get)
2. [Quick start: one UE, two gNBs, one handover](#2-quick-start-one-ue-two-gnbs-one-handover)
3. [Configuration](#3-configuration)
4. [The `xn-handover` command](#4-the-xn-handover-command)
5. [How it works](#5-how-it-works)
6. [Limitations](#6-limitations)
7. [What changed compared with UERANSIM](#7-what-changed-compared-with-ueransim)
8. [Credit and license](#8-credit-and-license)

---

## 1. What you get

Upstream UERANSIM has no handover. A UE can only change cell by losing it,
selecting another one and **registering again** with the core network. This
version adds a connected-mode handover between gNBs that are connected to the
same AMF:

1. **Preparation:** the source gNB sends the UE's context to the target gNB over
   a new **Xn** link between them.
2. **Execution:** the source gNB sends the UE an `RRCReconfiguration` with
   `reconfigurationWithSync`. The UE switches to the target cell while staying
   RRC-connected, and confirms with `RRCReconfigurationComplete`.
3. **Completion:** the target gNB sends an NGAP **Path Switch Request** to the
   AMF. The core moves the UE's downlink tunnel to the target and acknowledges.

You trigger a handover with one command:

```bash
nr-cli <source-gnb> -e "xn-handover <ue-id> <target-gnb-xn-ip>"
```

---

## 2. Quick start: one UE, two gNBs, one handover

Everything runs on one Ubuntu machine using loopback addresses (`127.0.0.x`
needs no network setup). The example configs are in
[`config/xn-handover-example/`](config/xn-handover-example/):

| File | Role | RLS / N2 / N3 IP | Xn listen | Cell ID (NCI) | `nr-cli` name |
|---|---|---|---|---|---|
| `gnb1.yaml` | source gNB | 127.0.0.1 | 127.0.0.1:38422 | `0x10` | `UERANSIM-gnb-999-70-1` |
| `gnb2.yaml` | target gNB | 127.0.0.2 | 127.0.0.2:38422 | `0x20` | `UERANSIM-gnb-999-70-2` |
| `ue1.yaml` | the UE, hears both gNBs | — | — | — | `imsi-999700000000001` |

All three use PLMN **999/70**, TAC **1**, slice **SST 1** and an AMF at
**127.0.0.5:38412**. These are the Open5GS defaults, so a fresh Open5GS needs no
config changes.

### Step 1: install Open5GS (the 5G core)

Open5GS is not part of this repository. Install it yourself, either way:

**Option A, Ubuntu package (simplest).** The network functions run as systemd
services and the user-plane interface `ogstun` is created for you:

```bash
sudo add-apt-repository ppa:open5gs/latest
sudo apt update
sudo apt install open5gs
```

**Option B, build from source.** Check the official guide for the current
dependency list: <https://open5gs.org/open5gs/docs/guide/02-building-open5gs-from-sources/>.

```bash
# MongoDB stores the subscribers. Install it as described in
# https://www.mongodb.com/docs/manual/installation/ , then:
sudo systemctl start mongod

sudo apt install -y python3-pip python3-setuptools python3-wheel ninja-build \
    build-essential flex bison git cmake meson libsctp-dev libgnutls28-dev \
    libgcrypt-dev libssl-dev libmongoc-dev libbson-dev libyaml-dev libnghttp2-dev \
    libmicrohttpd-dev libcurl4-gnutls-dev libtins-dev libtalloc-dev
git clone https://github.com/open5gs/open5gs
cd open5gs
meson build --prefix=`pwd`/install
ninja -C build
cd build && ninja install && cd ..

# user-plane TUN interface for the UPF
sudo ip tuntap add name ogstun mode tun
sudo ip addr add 10.45.0.1/16 dev ogstun
sudo ip link set ogstun up
```

**Check the AMF configuration.** It's in `/etc/open5gs/amf.yaml` for the
package, or `install/etc/open5gs/amf.yaml` for a source build. The values below
are the defaults; adjust them only if yours differ:

```yaml
amf:
  ngap:
    server:
      - address: 127.0.0.5        # = amfConfigs.address in the gNB configs
  guami:
    - plmn_id: { mcc: 999, mnc: 70 }
  tai:
    - plmn_id: { mcc: 999, mnc: 70 }
      tac: 1                      # = tac in the gNB configs
  plmn_support:
    - plmn_id: { mcc: 999, mnc: 70 }
      s_nssai:
        - sst: 1                  # = slices in the gNB configs
```

**Start the core.** Package install:

```bash
sudo systemctl restart 'open5gs-*'
```

Source build (start the NRF and SCP first):

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

**Add the example subscriber.** The IMSI, key and OPc are the ones in
`ue1.yaml`. Use the `open5gs-dbctl` script from the Open5GS source tree
(`misc/db/open5gs-dbctl`), or enter the same values in the Open5GS WebUI:

```bash
./misc/db/open5gs-dbctl add 999700000000001 \
    465B5CE8B199B49FAA5F0A2EE238A6BC E8ED289DEBA952E4283B54E88E6183CA
```

### Step 2: build UERANSIM_XN

```bash
sudo apt install -y make gcc g++ libsctp-dev lksctp-tools iproute2
sudo snap install cmake --classic          # CMake >= 3.17
git clone https://github.com/amir-ndr/UERANSIM_XN.git
cd UERANSIM_XN
make                                       # builds into build/
ls build/                                  # nr-gnb  nr-ue  nr-cli  ...
```

### Step 3: start gnb1, then the UE, then gnb2

Open three terminals in the `UERANSIM_XN` folder. **Start them in this order.**
Every simulated cell has the same signal strength, so a UE started with both
gNBs running picks one at random. Starting gnb2 *after* the UE has registered on
gnb1 makes gnb1 the source. The UE still hears gnb2 and learns its cell ID,
which the handover needs.

```bash
# Terminal 1: source gNB. Wait for:
#   "NG Setup procedure is successful"
#   "Xn server listening on 127.0.0.1:38422"
./build/nr-gnb -c config/xn-handover-example/gnb1.yaml

# Terminal 2: the UE (needs sudo to create its uesimtun0 interface). Wait for:
#   "Initial Registration is successful"
#   "PDU Session establishment is successful"
sudo ./build/nr-ue -c config/xn-handover-example/ue1.yaml

# Terminal 3: target gNB. Wait for:
#   "NG Setup procedure is successful"
#   "Xn server listening on 127.0.0.2:38422"
./build/nr-gnb -c config/xn-handover-example/gnb2.yaml
```

### Step 4: hand the UE over from gnb1 to gnb2

In a fourth terminal:

```bash
./build/nr-cli --dump                              # lists the two gNBs and the UE
./build/nr-cli UERANSIM-gnb-999-70-1 -e ue-list    # note the UE's ue-id on gnb1, e.g. 1
./build/nr-cli UERANSIM-gnb-999-70-1 -e "xn-handover 1 127.0.0.2"
# -> Xn handover triggered
```

### Step 5: check that it worked

```bash
./build/nr-cli UERANSIM-gnb-999-70-1 -e ue-list    # empty: the UE has left gnb1
./build/nr-cli UERANSIM-gnb-999-70-2 -e ue-list    # the UE: new ue-id, same amf-ngap-id
./build/nr-cli imsi-999700000000001 -e status      # still registered and CM-CONNECTED; current-cell changed
```

Every step is logged with the tag `[xn-ho]`:

| Where | Log lines, in order |
|---|---|
| gnb1 (source) | `SOURCE: handover requested` → `SOURCE: XnHandoverAck ok` → `SOURCE: RRCReconfiguration(reconfigurationWithSync) -> UE[…]` → `SOURCE: context released locally` |
| UE | `UE: handover command received, target nci=0x20` → `UE: serving cell … -> …, staying RRC-CONNECTED` → `UE: RRCReconfigurationComplete -> cell[…]` |
| gnb2 (target) | `TARGET: XnHandoverRequest from 127.0.0.1` → `TARGET: XnHandoverAck sent` → `TARGET: UE arrived` → `TARGET: PathSwitchRequest -> AMF` → `TARGET: COMPLETE … total=…ms` |

The last target line shows how long the handover took. A successful handover
does **not** produce a new registration in the UE or AMF logs.

**Hand it back** to gnb1 the same way, from gnb2, using the UE's id **on gnb2**:

```bash
./build/nr-cli UERANSIM-gnb-999-70-2 -e ue-list    # e.g. ue-id 1
./build/nr-cli UERANSIM-gnb-999-70-2 -e "xn-handover 1 127.0.0.1"
```

### Testing the user plane

To ping through the 5G user plane during a handover, run the UE in its own
network namespace. If the UE and the core share one namespace, the UE's address
(`10.45.0.x`) is local to the host, so downlink packets never go through the UPF
and the gNB. This applies to UERANSIM in general, not only to this version.

---

## 3. Configuration

### gNB

Two optional keys are added to the normal UERANSIM gNB config:

```yaml
xnAddress: 127.0.0.2      # IP the gNB listens on for Xn. Omit it to disable Xn on this gNB.
xnPort: 38422             # Xn listen port (default 38422)
```

For a handover between two gNBs:

- Both gNBs must be connected to **the same AMF**, with the **same TAC** and the
  **same slices**.
- The target gNB's `nci` must be **at most 1007** (`0x3EF`), because the cell ID
  is carried in the RRC field `physCellId` (see [Limitations](#6-limitations)).
- Each gNB needs its own `linkIp`/`ngapIp`/`gtpIp`, as usual in UERANSIM.

### UE

No new keys. The UE's `gnbSearchList` must contain the `linkIp` of **both** the
source and the target gNB, so the UE already hears the target cell before the
handover:

```yaml
gnbSearchList:
  - 127.0.0.1    # gnb1
  - 127.0.0.2    # gnb2
```

### More gNBs

To add a gNB, copy `gnb2.yaml` and give it a new `nci` (≤ 1007 if it can be a
handover target), new IP addresses and a new `xnAddress`. Then add its `linkIp`
to the UE's `gnbSearchList`.

---

## 4. The `xn-handover` command

```text
nr-cli <source-gnb-name> -e "xn-handover <ue-id> <target-xn-ip> [target-xn-port] [slice-sst]"
```

| Argument | Meaning |
|---|---|
| `<source-gnb-name>` | The gNB the UE is on now, e.g. `UERANSIM-gnb-999-70-1` (see `nr-cli --dump`). |
| `<ue-id>` | The UE's id **on that gNB**, from `ue-list`. |
| `<target-xn-ip>` | The target gNB's `xnAddress`. |
| `[target-xn-port]` | Optional, default `38422`. |
| `[slice-sst]` | Optional, default `1`; shown in the target's log. |

UE ids are local to each gNB, so after a handover the UE has a **new** `ue-id`
on the target. Its `amf-ngap-id` (shown by `ue-list`) stays the same, which is a
convenient way to follow one UE across handovers in a script.

---

## 5. How it works

This section follows one handover through the C++ code, so you can see exactly
what was added to UERANSIM and why. Section 5.1 gives the few UERANSIM basics you
need; section 5.2 is the step-by-step walkthrough; sections 5.3–5.5 show the
three messages in detail; section 5.6 lists the smaller supporting changes.

### 5.1 UERANSIM in one minute

UERANSIM runs each gNB and each UE as a process made of **tasks**. A task is a
thread with a message queue: another task gives it work by calling
`task->push(message)`, and the task handles its messages one at a time in its
`onLoop()` function. The messages are small C++ structs, defined in
`src/gnb/nts.hpp` for the gNB and `src/ue/nts.hpp` for the UE.

The gNB's tasks:

| Task | Class (folder) | Talks to |
|---|---|---|
| App / CLI | `GnbAppTask` (`src/gnb/app/`) | `nr-cli` |
| NGAP | `NgapTask` (`src/gnb/ngap/`) | the AMF, over N2 |
| RRC | `GnbRrcTask` (`src/gnb/rrc/`) | the UE's RRC layer |
| RLS | `GnbRlsTask` (`src/gnb/rls/`) | UEs, over the simulated radio link |
| GTP | `GtpTask` (`src/gnb/gtp/`) | the UPF, over N3 (user plane) |
| SCTP | `SctpTask` (`src/gnb/sctp/`) | carries NGAP to the AMF |
| **Xn (new)** | **`XnTask` (`src/gnb/xn/`)** | **other gNBs** |

The UE has matching tasks (`UeRrcTask`, `NasTask`, `UeRlsTask`, …) in `src/ue/`.

Two things UERANSIM already does make the handover possible:

- **Every gNB already knows the UE's "STI".** UERANSIM does not simulate real
  radio. It sends the radio messages over UDP, a layer it calls **RLS** (Radio
  Link Simulation). At start-up each UE picks a random 64-bit number, its
  **STI**, and sends heartbeats carrying it to *every* gNB in its
  `gnbSearchList`. Each gNB maps the STI to its own local `ue-id`. So the target
  gNB knows the UE's STI before the UE ever moves there, which makes the STI a
  shared name for the UE.
- **The UE already knows every cell's ID.** Each gNB broadcasts its NR Cell
  Identity (NCI) in its system information (SIB1), and the UE stores it for every
  cell it hears (`UeRrcTask::m_cellDesc`).

### 5.2 The handover, step by step

Below, **source** is the gNB the UE leaves and **target** is the gNB it moves to.
Step 0 happens long before the handover; steps 1–8 are the handover itself.

**Step 0: remember each PDU session's tunnels** (source, during normal session
setup).
`src/gnb/ngap/session.cpp`, `NgapTask::receiveSessionResourceSetupRequest`.
When the AMF sets up a PDU session, the gNB now also saves, per session, the
UPF's uplink tunnel (TEID and IP address), its own downlink TEID and the QoS flow
identifiers (QFIs) in a new map, `NgapUeContext::xnSessionInfo`. The target will
need these values later, and upstream UERANSIM does not keep them anywhere it can
read them back.

**Step 1: the command** (source).
`src/lib/app/cli_cmd.cpp` parses `xn-handover <ue-id> <target-ip> [port] [sst]`
into a new command type, `GnbCliCommand::XN_HANDOVER`.
`src/gnb/app/cmd_handler.cpp` checks that the UE exists, puts the request in a
new message, `NmGnbCliToNgap(TRIGGER_XN_HANDOVER)`, pushes it to the NGAP task,
and replies `Xn handover triggered`. The work is not done inside the CLI handler
because UERANSIM pauses the NGAP, RRC, RLS, GTP and SCTP tasks while any CLI
command is being handled; the handover needs them running.

**Step 2: preparation, source side** (source).
`src/gnb/ngap/handover.cpp`, `NgapTask::triggerXnHandover`:

1. Looks up the UE's STI with `rlsTask->stiOf(ueId)` (new, section 5.6).
2. Packs the saved sessions from step 0 into a text string (section 5.3).
3. Opens a TCP connection to the target's `xnAddress:xnPort`, sends an
   **XnHandoverRequest**, and waits for the answer (at most 30 s).
4. If the target is unreachable or rejects the request, it stops here and the UE
   stays where it is.

**Step 3: preparation, target side** (target).
`src/gnb/xn/task.cpp`, `XnTask::handleConnection`. The new `XnTask` is a TCP
server on its own thread. For each request it checks that the STI, the AMF UE
NGAP ID and at least one session are present, and then **stores the UE's context
without acting on it yet**: an `XnPendingHandover` entry in the map `m_pending`,
keyed by the STI. It answers with an **XnHandoverAck** that contains its own NCI,
so the source can tell the UE which cell to move to. Nothing is sent to the AMF
at this point.

**Step 4: the handover command** (source).
Back in `triggerXnHandover`, the source:

1. pushes `NmGnbNgapToRrc(XN_HANDOVER_COMMAND)`, carrying the target NCI, to its
   RRC task;
2. tells its GTP task to drop the UE (`UE_CONTEXT_RELEASE`);
3. deletes its own NGAP context for the UE.

The RRC task runs `GnbRrcTask::sendXnHandoverCommand` (`src/gnb/rrc/handover.cpp`).
It builds an `RRCReconfiguration` containing `reconfigurationWithSync`, the
standard NR handover command, with the target NCI as the target cell (section
5.4). It sends it to the UE and deletes its RRC context for the UE **without
sending an RRC Release**. An RRC Release would put the UE into idle mode, and it
would then register again.

**Step 5: the UE moves** (UE).
`src/ue/rrc/channel.cpp` now passes an incoming `RRCReconfiguration` to the new
`UeRrcTask::receiveRrcReconfiguration` (`src/ue/rrc/handover.cpp`), which:

1. decodes the command and reads the target NCI;
2. finds the cell with that NCI among the cells it hears (`m_cellDesc`);
3. makes it the serving cell: it updates `shCtx.currentCell` and tells its RLS
   task `ASSIGN_CURRENT_CELL`;
4. sends `RRCReconfigurationComplete`, which now goes to the target, because
   uplink RRC messages always go to the current cell.

It does **not** notify NAS and does not change its RRC state. If NAS saw the
cell change, it would start a registration procedure, which a handover must
avoid.

**Step 6: the target recognises the UE** (target).
`src/gnb/rrc/channel.cpp` now passes an incoming `RRCReconfigurationComplete`
(upstream: `// TODO`) to `GnbRrcTask::receiveRrcReconfigurationComplete`
(`src/gnb/rrc/handover.cpp`). It looks up the sender's STI, takes the matching
pending context from the Xn task (`xnTask->takePendingHandover(sti)`), creates an
RRC context for the UE, and pushes `NmGnbXnToNgap(TRIGGER_PATH_SWITCH)` to its
NGAP task. A completion with no pending handover is ignored.

**Step 7: the path switch request** (target → AMF).
`src/gnb/ngap/pathswitch.cpp`, `NgapTask::sendPathSwitchRequest`. It creates an
NGAP context for the UE under the UE's local `ue-id` on this gNB, with a new RAN
UE NGAP ID and the AMF UE NGAP ID received from the source. For each session it
allocates a new downlink tunnel on this gNB and sends the AMF a standard NGAP
**PathSwitchRequest** (section 5.5): "this UE is now here; send its downlink to
these tunnels". The AMF and SMF then move the UPF's downlink tunnel to the target.

**Step 8: the acknowledgement** (AMF → target).
`src/gnb/ngap/transport.cpp` now passes the AMF's
`PathSwitchRequestAcknowledge` (upstream: "unhandled") to
`NgapTask::receivePathSwitchRequestAcknowledge`. It registers the UE with the GTP
task and, for every session the AMF switched, creates the GTP session with the
uplink tunnel (updated if the AMF returned a new one) and the new downlink
tunnel. User-plane traffic now flows through the target. The target logs
`[xn-ho] TARGET: COMPLETE … total=…ms`.

The target saves the session tunnels in `xnSessionInfo` just like step 0, so the
UE can later be handed over again from the target.

### 5.3 The Xn messages

Xn here is a small protocol of its own, not the standard XnAP: each message is a
4-byte big-endian length followed by a JSON object, sent over TCP to the target's
`xnPort` (default 38422). The functions that send and receive it are at the top
of `src/gnb/xn/task.cpp` and `src/gnb/ngap/handover.cpp`.

**XnHandoverRequest** (source → target):

```json
{"type": "XnHandoverRequest", "ueId": 3, "sti": 7718283361553482752,
 "amfUeNgapId": 2, "ranUeNgapId": 3, "sliceSst": 1,
 "sourceGnb": "127.0.0.1",
 "sessions": "1,1,7f000007,1"}
```

| Field | Meaning |
|---|---|
| `sti` | the UE's RLS identity: how the target will recognise it |
| `amfUeNgapId` | the UE's ID at the AMF, which the target needs for the path switch |
| `ueId`, `ranUeNgapId`, `sourceGnb`, `sliceSst` | the source's local IDs and slice, for logs |
| `sessions` | every PDU session as `psi,upTeid,upAddrHex,qfi1+qfi2`, sessions separated by `\|`. Example: session 1, UPF uplink TEID 1, UPF address 127.0.0.7 (`7f000007`), QFI 1. |

**XnHandoverAck** (target → source):

```json
{"type": "XnHandoverAck", "ueId": 3, "sliceSst": 1, "status": "OK",
 "targetCell": "UERANSIM-gnb-999-70-2", "targetNci": 32, ...}
```

`targetNci` is the only field the source uses: it goes into the handover
command. A rejection has `"status": "REJECT"` and a `reason`.

### 5.4 The handover command (RRC)

NR signals a handover with an `RRCReconfiguration` that contains
`reconfigurationWithSync` (3GPP TS 38.331). The new code builds it with
UERANSIM's existing ASN.1 library. The nesting is the standard one:

```
RRCReconfiguration
└─ rrcReconfiguration  (RRCReconfiguration-IEs)
   └─ nonCriticalExtension  (RRCReconfiguration-v1530-IEs)
      └─ masterCellGroup: an encoded CellGroupConfig
         └─ spCellConfig
            └─ reconfigurationWithSync
               ├─ spCellConfigCommon.physCellId = target NCI   <- "move to this cell"
               ├─ newUE-Identity                = the UE's id (16 bits)
               └─ t304                          = 1000 ms
```

UERANSIM has no physical cell IDs, so the target's **NCI** is placed in the
`physCellId` field. That field only allows 0–1007, so a handover target's NCI
must be in that range. The source's RRC task refuses to send a command with a
larger NCI, but by then the source has already released the UE's NGAP and GTP
contexts (step 4), so the UE is left without a working connection: keep every
target NCI at 1007 or below. Two other
fields of `spCellConfigCommon` are filled with fixed values only because the
ASN.1 definition requires them; the UE ignores them.

### 5.5 The path switch (NGAP)

The **PathSwitchRequest** (3GPP TS 38.413) that the target sends to the AMF:

| Information element | Value | Set by |
|---|---|---|
| RAN-UE-NGAP-ID | a new ID allocated by the target | UERANSIM's existing send helper (`sendNgapUeAssociated`) |
| Source-AMF-UE-NGAP-ID | the UE's ID at the AMF, received from the source over Xn | new code |
| User Location Information | the target cell's NR-CGI and TAI | UERANSIM's existing send helper |
| UE Security Capabilities | fixed: NEA1/NEA2 and NIA1/NIA2 | new code |
| PDU Session Resource To Be Switched DL List | for each session: the target's new downlink tunnel (its `gtpIp`, or `gtpAdvertiseIp` if set, plus a new TEID) and the accepted QoS flows | new code |

The AMF answers with a **PathSwitchRequestAcknowledge** listing the switched
sessions; each one can carry an updated uplink tunnel, which the target uses in
step 8.

### 5.6 Supporting changes in upstream files

| Change | Where | Why |
|---|---|---|
| New messages between tasks: `NmGnbCliToNgap`, `NmGnbXnToNgap`, and `XN_HANDOVER_COMMAND` in `NmGnbNgapToRrc` | `src/gnb/nts.hpp`, `src/utils/nts.hpp` | Carry steps 1, 4 and 6 from one task to the next. |
| `stiOf(ueId)` returns the STI of a local UE | `src/gnb/rls/udp_task.*`, `src/gnb/rls/task.*` | Steps 2 and 6 look up the UE's STI. |
| A mutex (lock) around the RLS UE tables | `src/gnb/rls/udp_task.*` | `stiOf` is called from the NGAP and RRC threads while the RLS thread updates the tables. |
| The STI → `ue-id` mapping is kept when a UE's heartbeats stop for a moment | `src/gnb/rls/udp_task.cpp` | Upstream deletes it, so a UE that comes back gets a new `ue-id` and its RRC, NGAP and GTP contexts (stored under the old id) are lost. |
| Per-session tunnel bookkeeping (`xnSessionInfo`) and handover timestamps in `NgapUeContext` | `src/gnb/types.hpp`, `src/gnb/ngap/session.cpp` | Step 0. |
| Config keys `xnAddress` and `xnPort` | `src/gnb/types.hpp`, `src/gnb.cpp` | Read from the gNB YAML; without `xnAddress` the Xn server does not start. |
| The Xn task is created, started and stopped with the other tasks | `src/gnb/gnb.cpp` | |
| Routing of the new messages | `src/gnb/ngap/task.*`, `src/gnb/rrc/task.*`, `src/gnb/rrc/channel.cpp`, `src/ue/rrc/channel.cpp`, `src/ue/rrc/task.hpp`, `src/gnb/ngap/transport.cpp` | Upstream ignored `RRCReconfiguration` (UE), `RRCReconfigurationComplete` (gNB) and `PathSwitchRequestAcknowledge` (gNB). |
| The `xn-handover` CLI command | `src/lib/app/cli_cmd.*`, `src/gnb/app/cmd_handler.cpp` | Step 1. |
| AMF selection skips AMFs that are not connected, and falls back to any connected AMF when a message requests no slice or no connected AMF supports the requested one | `src/gnb/ngap/nnsf.cpp` | Upstream returned no AMF in those cases (for example for a Service Request without NSSAI). A robustness fix, not specific to Xn. |
| `utils::CurrentTimeMicros()` | `src/utils/common.*` | The handover takes a few milliseconds, so its phases are timed in microseconds. |

No build-file change is needed: the `CMakeLists.txt` files of the gNB and the UE
compile every `.cpp` file under `src/gnb/` and `src/ue/`, so the new files are
picked up automatically.

### 5.7 Design choices

| Choice | Why |
|---|---|
| **Xn is a small TCP + JSON protocol**, not the standard XnAP (TS 38.423). | It carries exactly what the target needs: the UE's AMF identity and its PDU sessions (uplink tunnel and QoS flows). |
| **The UE is identified across gNBs by its RLS STI.** | Both gNBs already know it (section 5.1), so the target can recognise the arriving UE. |
| **The target cell is identified by its NCI**, carried in the RRC `physCellId` field. | UERANSIM has no physical cell ID, and the UE already knows each gNB's NCI from SIB1. |
| **The path switch is sent only after the UE has arrived.** | The downlink is never moved to a gNB the UE has not reached. |
| **The source sends no RRC Release, and the UE's NAS layer is not told about the move.** | Either would make the UE register again, which is exactly what a handover avoids. |
| **A gNB keeps a UE's radio-link id when the UE briefly goes silent.** | A UE that comes back keeps its id, so its RRC, NGAP and GTP contexts stay valid. |

---

## 6. Limitations

- **No automatic trigger.** UEs send no measurement reports, so handovers are
  started with `nr-cli` (or a script that calls it).
- **Same AMF and tracking area only.** No AMF change, no N2-based (via-AMF)
  handover, no inter-TAC mobility.
- **Simplified Xn.** TCP + JSON rather than XnAP; no SN Status Transfer and no
  forwarding of downlink packets that arrive at the source during the handover.
- **Simplified security.** No K_gNB* key refresh (TS 33.501). The UE security
  capabilities in the Path Switch Request are fixed (NEA1/2, NIA1/2).
- **Simplified RRC.** The UE does not run timer T304 and does not use the new
  C-RNTI it is given. Once the target cell is in coverage the move cannot fail.
- **Cell ID range.** Handover targets must have an NCI between 0 and 1007
  (section 5.4).
- **One handover at a time per gNB pair.** The source's NGAP task waits for the
  Xn answer (up to 30 s) before doing anything else, and the target's Xn server
  handles one request at a time.

---

## 7. What changed compared with UERANSIM

All changes are in `src/`, plus the example configs. Section 5 explains what
each change does; this section only lists the files. To see every line, use this
comparison against the base commit:
<https://github.com/aligungr/UERANSIM/compare/b4157fa...amir-ndr:UERANSIM_XN:xn-handover>

**New files**

| File | Purpose |
|---|---|
| `src/gnb/xn/task.hpp`, `task.cpp` | The Xn server: receives handover requests and keeps each UE's context until the UE arrives. |
| `src/gnb/ngap/handover.cpp` | Source side: sends the Xn request and starts the handover. |
| `src/gnb/ngap/pathswitch.cpp` | Target side: NGAP Path Switch Request and Acknowledge. |
| `src/gnb/rrc/handover.cpp` | gNB RRC: sends the handover command; accepts the arriving UE. |
| `src/ue/rrc/handover.cpp` | UE RRC: executes the handover command. |
| `config/xn-handover-example/` | The configs used in the quick start. |

**Modified upstream files**

| Area | Files | Change |
|---|---|---|
| NGAP | `gnb/ngap/session.cpp`, `task.*`, `transport.cpp`, `nnsf.cpp` | Remembers each PDU session's tunnels for handover; routes the new messages; handles the Path Switch Acknowledge; falls back to any connected AMF when no slice is requested. |
| RRC | `gnb/rrc/channel.cpp`, `task.*`, `ue/rrc/channel.cpp`, `ue/rrc/task.hpp` | Routes `RRCReconfiguration` (UE) and `RRCReconfigurationComplete` (gNB), which upstream left as TODO. |
| RLS | `gnb/rls/udp_task.*`, `task.*` | Looks up a UE's STI; keeps a UE's id across short signal loss; adds a lock for access from other tasks. |
| Messages, config, CLI | `gnb/nts.hpp`, `utils/nts.hpp`, `gnb/types.hpp`, `gnb.cpp`, `gnb/gnb.cpp`, `lib/app/cli_cmd.*`, `gnb/app/cmd_handler.cpp` | New internal messages, the `xnAddress`/`xnPort` config keys, starting the Xn task, and the `xn-handover` command. |
| Utilities | `utils/common.*` | Microsecond timestamps for timing the handover. |

---

## 8. Credit and license

- **UERANSIM** is by **Ali Güngör** and contributors:
  <https://github.com/aligungr/UERANSIM>. All functionality other than the Xn
  handover (UE and gNB simulation, NAS, RRC, NGAP, GTP, the simulated radio
  link, the ASN.1 code) is upstream UERANSIM, unchanged except as listed in
  section 7.
- **The Xn handover extension** is by **Amir Naderi** (2026). The new source
  files carry his copyright header; modified upstream files keep their original
  header.
- **License:** GNU AGPL-3.0, the same as UERANSIM. See [`LICENSE`](LICENSE).
