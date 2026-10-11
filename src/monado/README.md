# Monado for the native Quest 1

`quest1-monado.patch` applies to Monado commit `cfa6078b8d7f368fa5296f540a436d59833e496c`
(`git apply quest1-monado.patch`). Build on the headset (Holo, -j2) with libhybris in /opt/hybris.

- `drivers/quest1`: 3DoF HMD from the SyncBoss IMU (Meta's libsyncboss.so through libhybris),
  Meta's Quest 1 lens distortion (CatmullRom10, docs/quest1-lens.md), views rotated 180°.
  The four tracking cameras stream on demand (src/qcam sequence on the driver's SyncBoss handle,
  auto exposure) to the root-only shared memory `/quest1-cameras` while a reader keeps a heartbeat
  there; layout in `quest1_cameras_shm.h`, `QUEST1_CAMERAS=0` turns it off. `src/q1passthrough`
  shows them. The same memory carries the raw 1 kHz IMU samples and, per frameset, the SyncBoss
  shutter time (mid-exposure, on the IMU clock) for SLAM; `src/q1record` records both as a EuRoC
  dataset. `QUEST1_CAMERA_EXPOSURE_MAX_US` caps the auto exposure (default 14000; about 6000 for
  less motion blur), `QUEST1_CAMERA_EXPOSURE_US` fixes it.
- `compositor/main/comp_window_mdp.c`: zero-copy target, gralloc buffers scanned out with
  MSMFB_ATOMIC_COMMIT, vsync pacing, panels/GPU governor switched with activity.
- `XRT_HYBRIS_AHB`: AHardwareBuffer swapchains through libhybris (needs the Android
  hwservicemanager, graphics allocator and configstore daemons: device/holo-android-hals.sh).
- `qbridge`: older experiment (Monado frames handed to Meta's runtime under Android), unused natively.

Install: device/holo-monado.sh. Apps: `xr-run <app>`.
