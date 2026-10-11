# Monado for the native Quest 1

`quest1-monado.patch` applies to Monado commit `cfa6078b8d7f368fa5296f540a436d59833e496c`
(`git apply quest1-monado.patch`); `src/` holds the same files as the patch for those it adds or
replaces, and is the source of truth (regenerate their patch sections from a tree that contains
them). Build in the local Holo rootfs with `tools/build-holo-monado.sh` (through
`tools/run-holo-local.sh`). The SLAM-enabled tree is `build/steamos/sources/monado-slam`, build
directory `build-slam`: `MONADO_SRC=/build-sources/monado-slam MONADO_BUILD_DIR=build-slam
MONADO_SLAM=1`. That `monado-service` needs OpenCV at runtime (the image installs it with Basalt).

- `drivers/quest1`: 3DoF HMD from the SyncBoss IMU (Meta's libsyncboss.so through libhybris),
  Meta's Quest 1 lens distortion (CatmullRom10, docs/quest1-lens.md), views rotated 180°.
  The four tracking cameras stream on demand (src/qcam sequence on the driver's SyncBoss handle,
  auto exposure) to the root-only shared memory `/quest1-cameras` while a reader keeps a heartbeat
  there; layout in `quest1_cameras_shm.h`, `QUEST1_CAMERAS=0` turns it off. `src/q1passthrough`
  shows them. The same memory carries the raw 1 kHz IMU samples and, per frameset, the SyncBoss
  shutter time (mid-exposure, on the IMU clock) for SLAM; `src/q1record` records both as a EuRoC
  dataset. `QUEST1_CAMERA_EXPOSURE_MAX_US` caps the auto exposure (default 14000; about 6000 for
  less motion blur), `QUEST1_CAMERA_EXPOSURE_US` fixes it.
- 6DoF head tracking (`quest1_slam.c`, `quest1_calib.c`): Monado's SLAM tracker with Basalt
  (VIT, `/opt/basalt/lib/libbasalt.so` unless `VIT_SYSTEM_LIBRARY_PATH` says otherwise) on the
  HEADSET framesets of the bottom stereo pair (`QUEST1_SLAM_CAMS=0,2`; `0,2,1,3` for all four,
  cam0/cam1 must stay 0,2) and the 1 kHz IMU. Calibration from `/persist/calibration` (newest
  `online/<id>`, else the factory files): KB4 fitted to Fisheye62, IMU rectified into device axes
  with the temperature-dependent offsets, no calibration file for Basalt. Frames (SyncBoss shutter
  time) and IMU samples share one MCU-to-monotonic offset. The cameras stream all the time while
  SLAM runs; SLAM gets every second frameset (`QUEST1_SLAM_FRAME_DIVIDER=2`, 15 Hz: Basalt's
  optical flow cannot keep up with 30 Hz on the SD835). The pose falls back to 3DoF until Basalt's
  first pose, and while a diverged tracker (faster than 3 m/s or 10 m from its origin) is reset;
  `QUEST1_SLAM=0` turns SLAM off (also off when the VIT library does not load, or in builds without
  `XRT_FEATURE_SLAM`). `t_slam_reset` (added to `t_tracker_slam.cpp`) does the reset.
  Monado's `SLAM_*` variables work (`SLAM_WRITE_CSVS=1 SLAM_CSV_PATH=...` for timing/trajectory
  CSVs). The 3DoF fusion uses the factory rectification too; `QUEST1_IMU_AXES` still overrides it.
  Under `hybris-run` the loader takes libhybris's `libOpenCL.so.1` and `libEGL.so.1` from
  `/opt/hybris/lib` for ffmpeg and Qt (pulled in by OpenCV); that only prints "no version
  information available" and works.
- `compositor/main/comp_window_mdp.c`: zero-copy target, gralloc buffers scanned out with
  MSMFB_ATOMIC_COMMIT, vsync pacing, panels/GPU governor switched with activity.
- `XRT_HYBRIS_AHB`: AHardwareBuffer swapchains through libhybris (needs the Android
  hwservicemanager, graphics allocator and configstore daemons: device/holo-android-hals.sh).
- `qbridge`: older experiment (Monado frames handed to Meta's runtime under Android), unused natively.

Install: device/holo-monado.sh. Apps: `xr-run <app>`.
