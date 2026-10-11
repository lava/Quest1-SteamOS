#!/usr/bin/env python3
"""Prepare a q1record dataset and the Quest 1 calibration for Basalt.

Usage: quest1-basalt-prep.py [--cams 0,2] [--factory] CALIB_DIR RECORDING OUT

CALIB_DIR is a copy of the headset's /persist/calibration (stays on the host; never commit it).
RECORDING is a q1record output directory. OUT receives:

  OUT/mav0/cam<i>/        symlinks to the selected cameras, renumbered from 0, with exposure.csv
  OUT/mav0/imu0/data.csv  IMU samples rectified with the factory calibration (device axes:
                          +X right, +Y up, +Z back), rect = R * (raw - offset(temperature))
  OUT/calib.json          Basalt calibration: KB4 intrinsics fitted to Meta's Fisheye62,
                          T_imu_cam = inv(DeviceFromImu) * DeviceFromCamera, IMU noise

Cameras and IMU offsets come from the newest online calibration (CALIB_DIR/online/*, refined by
Meta's tracking) unless --factory selects camera_calibration_v2.json / imu_calibration.json.
The IMU noise values are the Rift S driver's (rift_s_tracker.c) until we measure our own.
"""

import argparse
import csv
import json
import math
import os
import sys
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray
from scipy.optimize import least_squares
from scipy.spatial.transform import Rotation

Array = NDArray[np.float64]

RIFT_S_NOISE = {
    "accel_noise_std": [0.016] * 3,
    "gyro_noise_std": [0.000282] * 3,
    "accel_bias_std": [0.001] * 3,
    "gyro_bias_std": [0.0001] * 3,
}


def fisheye62_project(theta: Array, phi: Array, f: float, cx: float, cy: float, d: Array) -> Array:
    """Pixel of rays at angle theta from the optical axis, azimuth phi (as rift_s_util.cpp)."""
    t2 = theta * theta
    radial = 1 + t2 * (d[0] + t2 * (d[1] + t2 * (d[2] + t2 * (d[3] + t2 * (d[4] + t2 * d[5])))))
    xp, yp = theta * np.cos(phi), theta * np.sin(phi)
    p1, p2 = d[6], d[7]
    dx = 2 * p1 * xp * yp + p2 * (t2 + 2 * xp * xp)
    dy = 2 * p2 * xp * yp + p1 * (t2 + 2 * yp * yp)
    return np.stack([f * (xp * radial + dx) + cx, f * (yp * radial + dy) + cy], axis=-1)


def kb4_project(theta: Array, phi: Array, f: float, cx: float, cy: float, k: Array) -> Array:
    t2 = theta * theta
    r = theta * (1 + t2 * (k[0] + t2 * (k[1] + t2 * (k[2] + t2 * k[3]))))
    return np.stack([f * r * np.cos(phi) + cx, f * r * np.sin(phi) + cy], axis=-1)


def fit_kb4(cam: dict[str, Any]) -> tuple[dict[str, float], float, float]:
    """KB4 with the same f, cx, cy, fitted over the rays that land in the image."""
    f, cx, cy = cam["Projection"]["Coefficients"]
    d = np.array(cam["Distortion"]["Coefficients"], dtype=np.float64)
    w, h = cam["ImageSize"]
    theta, phi = np.meshgrid(np.linspace(0, math.radians(100), 200), np.linspace(-math.pi, math.pi, 72))
    px = fisheye62_project(theta, phi, f, cx, cy, d)
    # Only rays inside the image, and only while the projection still grows with theta.
    r = np.hypot(px[..., 0] - cx, px[..., 1] - cy)
    monotonic = np.cumprod(np.diff(r, axis=1, prepend=-1.0) > 0, axis=1).astype(bool)
    inside = (px[..., 0] >= 0) & (px[..., 0] <= w - 1) & (px[..., 1] >= 0) & (px[..., 1] <= h - 1)
    keep = monotonic & inside
    th, ph, target = theta[keep], phi[keep], px[keep]

    def residual(k: Array) -> Array:
        res: Array = (kb4_project(th, ph, f, cx, cy, k) - target).ravel()
        return res

    fit = least_squares(residual, x0=np.array(d[:4]))
    err = np.linalg.norm(residual(fit.x).reshape(-1, 2), axis=1)
    k = fit.x
    intr = {"fx": f, "fy": f, "cx": cx, "cy": cy, "k1": k[0], "k2": k[1], "k3": k[2], "k4": k[3]}
    return intr, float(np.sqrt(np.mean(err**2))), float(err.max())


def mat4(values: list[float]) -> Array:
    return np.array(values, dtype=np.float64).reshape(4, 4)


def pose_json(t: Array) -> dict[str, float]:
    q = Rotation.from_matrix(t[:3, :3]).as_quat()  # x, y, z, w
    return {"px": t[0, 3], "py": t[1, 3], "pz": t[2, 3], "qx": q[0], "qy": q[1], "qz": q[2], "qw": q[3]}


def offset_at(model: dict[str, Any], temperature: Array) -> Array:
    """Raw-frame offsets, (N, 3), for each temperature."""
    kind = model["Model"]
    if kind == "Constant":
        return np.tile(np.array(model["ConstantOffset"], dtype=np.float64), (len(temperature), 1))
    if kind == "LinearTemperatureDependence":
        o0 = np.array(model["OffsetAtZeroDegC"], dtype=np.float64)
        c = np.array(model["OffsetTemperatureCoefficient"], dtype=np.float64)
        offsets: Array = o0 + temperature[:, None] * c
        return offsets
    if kind == "TemperaturePiecewiseLinear":
        if model.get("CoordinateSystem", "Raw") != "Raw":
            raise ValueError(f"offsets in {model['CoordinateSystem']} coordinates")
        pairs = np.array(model["TemperatureOffsetPairs"], dtype=np.float64)
        pairs = pairs[np.argsort(pairs[:, 0])]
        return np.stack([np.interp(temperature, pairs[:, 0], pairs[:, i]) for i in (1, 2, 3)], axis=1)
    raise ValueError(f"unknown IMU offset model {kind}")


def load_calibration(calib_dir: Path, factory: bool) -> tuple[dict[str, Any], dict[str, Any], str]:
    online = sorted((calib_dir / "online").glob("*"), key=lambda p: p.stat().st_mtime)
    if not factory and online:
        d = json.loads(online[-1].read_text())
        return d, d["ImuCalibration"], str(online[-1])
    cams = json.loads((calib_dir / "camera_calibration_v2.json").read_text())
    imu = json.loads((calib_dir / "imu_calibration.json").read_text())["ImuCalibration"]
    return cams, imu, "factory"


def read_csv(path: Path) -> Array:
    rows = [r for r in csv.reader(path.open()) if r and not r[0].startswith("#")]
    return np.array(rows, dtype=np.float64)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cams", default="0,2", help="calibration ids in Basalt order (default 0,2)")
    ap.add_argument("--factory", action="store_true", help="factory instead of online calibration")
    ap.add_argument("calib_dir", type=Path)
    ap.add_argument("recording", type=Path)
    ap.add_argument("out", type=Path)
    args = ap.parse_args()
    cam_ids = [int(c) for c in args.cams.split(",")]

    cams_json, imu_cal, source = load_calibration(args.calib_dir, args.factory)
    by_id = {int(c["Id"]): c for c in cams_json["CameraCalibration"]}
    device_from_imu = mat4(imu_cal["DeviceFromImu"])
    imu_from_device = np.linalg.inv(device_from_imu)
    print(f"calibration: {source}", file=sys.stderr)

    rec = args.recording / "mav0"
    out = args.out / "mav0"
    (out / "imu0").mkdir(parents=True, exist_ok=True)

    t_imu_cam, intrinsics, resolution = [], [], []
    frames = read_csv(rec / "frames.csv")
    for n, cid in enumerate(cam_ids):
        cam = by_id[cid]
        intr, rms, worst = fit_kb4(cam)
        print(f"cam{n} = id {cid}: KB4 fit rms {rms:.3f} px, max {worst:.3f} px", file=sys.stderr)
        intrinsics.append({"camera_type": "kb4", "intrinsics": intr})
        resolution.append(cam["ImageSize"])
        t_imu_cam.append(pose_json(imu_from_device @ mat4(cam["DeviceFromCamera"])))
        link = out / f"cam{n}"
        if link.is_symlink() or link.exists():
            link.unlink()
        src = (rec / f"cam{cid}").resolve()
        link.mkdir()
        (link / "data").symlink_to(src / "data")
        (link / "data.csv").symlink_to(src / "data.csv")
        with (link / "exposure.csv").open("w") as fo:
            fo.write("#timestamp [ns],exposure [ns]\n")
            for row in frames:
                fo.write(f"{int(row[0])},{int(row[4 + cid]) * 1000}\n")
    for stale in sorted(out.glob("cam*")):
        if stale.name not in {f"cam{n}" for n in range(len(cam_ids))}:
            print(f"warning: stale {stale}", file=sys.stderr)

    imu = read_csv(rec / "imu0" / "data.csv")
    temps = read_csv(rec / "imu0" / "temperature.csv")
    temperature = np.interp(imu[:, 0], temps[:, 0], temps[:, 1])
    gyro_r = np.array(imu_cal["Gyroscope"]["RectificationMatrix"], dtype=np.float64).reshape(3, 3)
    accel_r = np.array(imu_cal["Accelerometer"]["RectificationMatrix"], dtype=np.float64).reshape(3, 3)
    gyro = (imu[:, 1:4] - offset_at(imu_cal["Gyroscope"]["Offset"], temperature)) @ gyro_r.T
    accel = (imu[:, 4:7] - offset_at(imu_cal["Accelerometer"]["Offset"], temperature)) @ accel_r.T
    with (out / "imu0" / "data.csv").open("w") as fo:
        fo.write(
            "#timestamp [ns],w_RS_S_x [rad s^-1],w_RS_S_y [rad s^-1],w_RS_S_z [rad s^-1],"
            "a_RS_S_x [m s^-2],a_RS_S_y [m s^-2],a_RS_S_z [m s^-2]\n"
        )
        for t, g, a in zip(imu[:, 0], gyro, accel):
            fo.write(f"{int(t)},{g[0]:.9g},{g[1]:.9g},{g[2]:.9g},{a[0]:.9g},{a[1]:.9g},{a[2]:.9g}\n")
    still = np.linalg.norm(gyro[: min(len(gyro), 2000)], axis=1)
    print(
        f"imu: {len(imu)} samples, {temperature.min():.1f}-{temperature.max():.1f} °C, "
        f"|gyro| over the first 2 s {still.mean():.4f} rad/s, |accel| {np.linalg.norm(accel, axis=1).mean():.3f}",
        file=sys.stderr,
    )

    calib = {
        "value0": {
            "T_imu_cam": t_imu_cam,
            "intrinsics": intrinsics,
            "resolution": resolution,
            "calib_accel_bias": [0.0] * 9,
            "calib_gyro_bias": [0.0] * 12,
            "imu_update_rate": 1000.0,
            **RIFT_S_NOISE,
            "cam_time_offset_ns": 0,
            "vignette": [],
        }
    }
    (args.out / "calib.json").write_text(json.dumps(calib, indent=4) + "\n")
    print(f"wrote {args.out}/calib.json and {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    os.umask(0o022)
    sys.exit(main())
