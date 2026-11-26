# CueKeeper

CueKeeper is a small show controller. The stage manager sends `go` to a separate C engine. The engine owns the scene and continues dispatching if the terminal controller exits. `status` reads the current scene and cue history without advancing or replaying it. A second `go` during the 30-second scene exits with status `2`. [QNX Global Hackathon](https://qnx.devpost.com/)

| Offset from Go | Cue ID | Simulated output |
| ---: | --- | --- |
| 5 s | `lx-1` | Set the stage lights to blue |
| 15 s | `snd-1` | Play the opening chime |
| 25 s | `cap-1` | Display “Welcome to the show.” |

The `opening` scene is compiled into `src/scene.c`. Each simulation emits one line to the engine log. Status reports the cue ID, scheduled and actual dispatch offsets from Go, and lateness (`actual − scheduled`) in milliseconds. The engine records time in nanoseconds and samples `CLOCK_MONOTONIC` immediately before each simulated dispatch. The completed run stays in memory until another Go or until the engine stops.

On QNX, `src/engine_qnx.c` arms a one-shot `CLOCK_MONOTONIC` timer with `TIMER_ABSTIME` and receives `SIGEV_PULSE` expirations on a native QNX channel. Go and Status use native `MsgSend`/`MsgReceive`/`MsgReply` on the local `cuekeeper` service. The terminal controller holds no scene state. [QNX timer API](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html) · [QNX local service API](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html)

## Build and boot QNX

Target **QNX OS 8.0 on x86-64 QEMU**. Build with **QNX SDP 8.0 `qcc -Vgcc_ntox86_64`**, using the GCC version supplied by the installed SDP. The example uses an x86-64 Ubuntu development host with QEMU, GNU Make, and OpenSSH. Obtain a QNX Everywhere license and install the x86-64 QNX SDP 8.0 and virtual target tooling through QNX Software Center. The commands assume the SDK is installed at `~/qnx800`. [QNX Everywhere license](https://www.qnx.com/getqnx) · [QNX installation guide](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxsdp.quickstart/topic/install_host.html) · [qcc reference](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/q/qcc.html)

From the CueKeeper directory on the Ubuntu host, with no existing guest for this demo:

```bash
source "$HOME/qnx800/qnxsdp-env.sh"
qcc --version
qemu-system-x86_64 --version
make qnx

vm_dir=$(mktemp -d /tmp/cuekeeper-qnx.XXXXXX)
(cd "$vm_dir" && mkqnximage --type=qemu --arch=x86_64 --run=-h)
guest=$(cd "$vm_dir" && mkqnximage --getip)
```

If you already have an x86-64 QNX 8.0 QEMU guest, set `guest` to its reachable IP and use the same deployment commands. The example uses the isolated demo guest's root account so the engine can register its public IPC channel. On a guest with a restricted security policy, the engine account needs `PROCMGR_AID_PUBLIC_CHANNEL` and any path permissions required for `/dev/name/local/cuekeeper`. [QNX `mkqnximage` guide](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html) · [QNX `name_attach` requirements](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html)

```bash
ssh "root@$guest" 'mkdir -p /data/cuekeeper'
scp build/qnx-x86_64/cuekeeper-engine build/qnx-x86_64/cuekeeperctl tests/smoke.sh \
    "root@$guest:/data/cuekeeper/"
ssh "root@$guest" 'cd /data/cuekeeper && chmod +x cuekeeper-engine cuekeeperctl'
```

## Run the demo

In a QNX target terminal, keep the engine running:

```sh
cd /data/cuekeeper
./cuekeeper-engine
```

In a second target terminal, start the controller:

```sh
cd /data/cuekeeper
./cuekeeperctl
```

Type `go`. After the engine prints the lighting dispatch, type `status` and then `quit`. The engine prints the sound and caption dispatches in its own terminal. Reconnect with `./cuekeeperctl status`. The one-shot commands are `./cuekeeperctl go` and `./cuekeeperctl status`.

## Run the QEMU smoke test

Stop any manual CueKeeper engine before starting the test. The test starts its own engine, sends Go, checks that a second Go is rejected, and exits the original controller after observing only `lx-1`. It waits for the scene to finish and then checks the reconnected history and simulation log for all three cues exactly once in order. A second Status must return the same completed history. The guest needs `sh`, anonymous pipes, and the standard `cat`, `cmp`, `mkdir`, `rm`, `rmdir`, and `sleep` utilities.

From the Ubuntu host:

```bash
ssh "root@$guest" 'uname -a && cd /data/cuekeeper && sh smoke.sh .'
```

Save the QNX guest's `uname -a`, host `qcc --version` and `qemu-system-x86_64 --version`, and the three timing rows from the smoke test for the demo record. The lateness rows describe that guest run under its specific host load. To stop a guest started with the commands above, run `(cd "$vm_dir" && mkqnximage --stop)` on the Ubuntu host.

## Run the Linux smoke test

From the CueKeeper directory on Linux or WSL, run `make test`. This builds a Linux adapter that uses a monotonic `timerfd` and local Unix datagram IPC, then runs the same shell test against the shared timeline and CLI. The shell test chooses its own socket. For a manual Linux run, set `CUEKEEPER_SOCKET` to a socket pathname in a private temporary directory in both terminals, and use the executables under `build/linux` after `make portable`.

**Milestone verification.** The smoke test passed on x86-64 Kali Linux under WSL2 with GCC 16.2.0 and GNU Make 4.4.1. In one Linux run, measured lateness was 0.069 ms for `lx-1`, 0.101 ms for `snd-1`, and 0.113 ms for `cap-1`.

QEMU 11.1.0 is available in that WSL distribution. No bootable QNX guest image was found, and the native build stopped at `qcc: not found`. Native compilation, QNX execution, and QEMU timing measurements are pending.

The hackathon calls for a reproducible QNX build or run, a public repository with an open-source license, and a short recording running on the target. This milestone provides the source and demo scene. Capture the guest run and choose a repository license before submission. [Event rules](https://qnx.devpost.com/rules)
