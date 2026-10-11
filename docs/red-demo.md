# K230 red-screen presentation mode

This is an explicit detector/response demonstration, **not AI content moderation**.
Normal analysis is left unchanged. Only `dev.k230.red_demo` may be force-stopped;
the existing executor revalidates foreground and Android's stopped state before
opening Mentor's puzzle. It never falls back to stopping an arbitrary foreground app.

## Behavior

1. scrcpy captures the authorized phone on little Linux, fixed orientation.
2. Big Linux hardware VPU decodes the video; no software decoding or KPU worker.
3. A neutral frame (at most 20% red) arms the detector. The first subsequent frame
   with at least 80% bright red triggers once, then requires another neutral frame.
4. The little-core executor stops the dedicated test application, checks stopped
   state, and starts `dev.k230.mentor_app/.MvpBreakActivity`.

The RGB red box is R >= 200, G <= 60, B <= 60. I420/NV12 conversions honor BT.601/
BT.709 and limited/full range; compressed-color tolerance is intentional.
Sampled color coverage is approximate. Detection events older than 500ms, video
gaps, missing capture, wrong/multiple phones, and changed foregrounds fail closed.
Mentor Accessibility must be enabled manually. An ADB start ACK is **not** visual
proof; measure the full red-screen-to-puzzle delay on the physical phone.

## Build

Use the existing host/big/little CMake configurations:

```sh
cmake --build build --target k230_tests -j4
build/tests/k230_tests --gtest_filter='RedDemo.*:ForceStopExecutor.*'
python3 tests/red_demo_service_test.py
cmake --build build-big --target k230-red-demo-detector -j4
cmake --build build-little --target k230-mvp-force-stop-probe -j4
```

Build the small test APK on Windows, with the existing SDK/JDK:

```powershell
& .\tools\red-demo-android\build.ps1
```

The generated signing key is for local demonstration only, kept in the ignored
build directory. The app has no permissions and only renders neutral/red/blue
screens; it cannot perform or simulate the stop or puzzle action itself.

## Deploy / run on the board

Keep a backup of the current binaries. Install the test APK without clearing
Mentor's data. Copy these artifacts to `/root/rafiq`:

- `tools/k230_red_demo.py` (alongside the existing `k230_dual_linux_service.py`)
- `build-big/inspector/k230-red-demo-detector` -> `bin/k230-red-demo-detector`
- `build-little/bridge/k230-mvp-force-stop-probe` -> little Linux `bin/k230-mvp-force-stop-probe`

Stop the normal service and wait for its process/bridge to exit before starting
demo mode. Both modes use the same exclusive service lock.

```sh
python3 /root/rafiq/k230_dual_linux_service.py --stop
# Verify the normal service has exited, then:
python3 /root/rafiq/k230_red_demo.py --phone 10ACCU3C5J002BG --daemon
tail -f /root/rafiq/red-demo.log
```

Open **K230 — عرض تجريبي** on the phone. Wait for `event: ready`, then press
the red-screen button. The blue-screen button is a negative control and must
not cause a stop. Tap blue to return to the neutral screen. Repeat by relaunching
the test app after solving the puzzle. Do not rotate the phone during the demo.

Rollback (does not reflash or modify the board image):

```sh
python3 /root/rafiq/k230_red_demo.py --stop
# Verify the demo has exited, then:
python3 /root/rafiq/k230_dual_linux_service.py --daemon
```

Normal mode now accepts explicitly marked foreground-only layout snapshots,
and its AI2D adapter owns builders and tensors with C++ RAII. Board stress tests
passed 600 NSFW and 200 UI inferences with stable resource counts after warmup.
The automatic K230-to-Vivo stop/puzzle path and its latency are still unverified;
do not represent this demo as a validated production protection system.
