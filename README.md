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

```
 nr-cli                  SOURCE gNB                     TARGET gNB                   UE              AMF / SMF / UPF
 ──────                  ──────────                     ──────────                   ──              ───────────────
 xn-handover ──────────► reads the UE's sessions
                         and radio-link id (STI)
                         XnHandoverRequest ──────────► stores the UE context
                                                       under the STI
                         ◄────────────── XnHandoverAck (target cell ID)
                         RRCReconfiguration ───────────────────────────────────────► switches to the
                         (reconfigurationWithSync)                                    target cell,
                         releases its UE context                                      stays CONNECTED
                         (no RRC Release)
                                                       ◄─────────── RRCReconfigurationComplete
                                                       finds the context by STI
                                                       PathSwitchRequest ──────────────────────────────► moves the
                                                       (new downlink tunnel)                              downlink tunnel
                                                       ◄─────────────────────── PathSwitchRequestAcknowledge
                                                       sets up the PDU sessions
```

Design choices:

| Choice | Why |
|---|---|
| **Xn is a small TCP + JSON protocol**, not the standard XnAP (TS 38.423). | It carries exactly what the target needs: the UE's AMF identity and its PDU sessions (uplink tunnel and QoS flows). |
| **The UE is identified across gNBs by its RLS STI.** | In UERANSIM's simulated radio link, every UE has a random 64-bit identity (STI) that it sends to every gNB it hears. Both gNBs already know it, so the target can recognise the arriving UE. |
| **The target cell is identified by its NCI**, carried in the RRC `physCellId` field. | UERANSIM has no physical cell ID, and the UE already knows each gNB's NCI from its system information (SIB1). |
| **The path switch is sent only after the UE has arrived.** | The downlink is never moved to a gNB the UE has not reached. |
| **The source sends no RRC Release, and the UE's NAS layer is not told about the move.** | Either would make the UE re-register, which is exactly what a handover avoids. |
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
- **Cell ID range.** Handover targets must have an NCI between 0 and 1007.

---

## 7. What changed compared with UERANSIM

All changes are in `src/`, plus the example configs. To see them exactly, use
this comparison against the base commit:
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

No build-system changes are needed: the gNB's CMake file picks up the new
source files automatically.

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
