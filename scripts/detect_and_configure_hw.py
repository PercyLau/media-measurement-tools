#!/usr/bin/env python3
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "configs" / "experiment.json"


def run(cmd):
    try:
        p = subprocess.run(cmd, shell=True, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return p.stdout + p.stderr
    except subprocess.CalledProcessError as e:
        return e.stdout + e.stderr


def ffmpeg_has_encoder(name):
    if not shutil.which("ffmpeg"):
        return False
    out = run("ffmpeg -hide_banner -encoders 2>&1 || true")
    return name in out


def nv_present():
    # Prefer nvidia-smi, fallback to ffmpeg encoder presence or libcuda
    if shutil.which("nvidia-smi"):
        return True
    if ffmpeg_has_encoder("h264_nvenc") or ffmpeg_has_encoder("hevc_nvenc"):
        return True
    out = run("ldconfig -p 2>/dev/null | grep -i libcuda || true")
    return "libcuda" in out


def v4l2_decode_present():
    # Check /dev/video* and list formats for H264/HEVC capture support
    if not shutil.which("v4l2-ctl"):
        # presence of /dev/video* still useful
        return any(Path("/dev").glob("video*"))
    devs = sorted([p.name for p in Path("/dev").glob("video*")])
    for dev in devs:
        out = run(f"v4l2-ctl -d /dev/{dev} --list-formats-ext 2>&1 || true")
        if "H264" in out or "HEVC" in out or "h264" in out or "hevc" in out:
            return True
    return False


def vaapi_present():
    return bool(shutil.which("vainfo"))


def backup_config(path: Path):
    ts = time.strftime("%Y%m%dT%H%M%S")
    bak = path.with_suffix(f".bak.{ts}")
    shutil.copy2(path, bak)
    return bak


def main(argv):
    apply_changes = "--apply" in argv
    if not CONFIG.exists():
        print("configs/experiment.json not found", file=sys.stderr)
        return 2

    with open(CONFIG, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    found_nv = nv_present()
    found_v4l2 = v4l2_decode_present()
    found_vaapi = vaapi_present()

    summary = {
        "nvidia_nvenc": found_nv,
        "v4l2_mem2mem": found_v4l2,
        "vaapi": found_vaapi,
    }

    # Apply policy: prefer NV for encoding, prefer V4L2 for decoding on ARM
    enc_place = cfg.setdefault("encoder", {}).setdefault("hardware_encoder_placeholder", {})
    dec_place = cfg.setdefault("receiver", {}).setdefault("hardware_decoder_placeholder", {})

    enc_place["enabled"] = bool(found_nv)
    if found_nv:
        cfg.setdefault("encoder", {}).setdefault("hardware_encoders", {})["h264"] = "nvh264enc"
    else:
        # prefer software
        enc_place["enabled"] = False

    dec_place["enabled"] = bool(found_v4l2)
    if found_v4l2:
        cfg.setdefault("receiver", {}).setdefault("hardware_decoders", {})["h264"] = "v4l2h264dec"
    else:
        dec_place["enabled"] = False

    # VAAPI: only used as secondary hint
    if found_vaapi and not found_nv:
        # if VAAPI available and no NV, consider using VAAPI encoder
        cfg.setdefault("encoder", {}).setdefault("hardware_encoders", {})["h264"] = "vaapih264enc"

    print("Detection summary:")
    for k, v in summary.items():
        print(f" - {k}: {v}")

    print("\nPlanned changes to configs/experiment.json:")
    print(json.dumps({"encoder.hardware_encoder_placeholder.enabled": enc_place.get("enabled"),
                      "receiver.hardware_decoder_placeholder.enabled": dec_place.get("enabled")}, indent=2))

    if apply_changes:
        bak = backup_config(CONFIG)
        with open(CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2, ensure_ascii=False)
        print(f"Applied changes; backup saved to {bak}")
    else:
        out = CONFIG.with_name(CONFIG.stem + ".detected.json")
        with open(out, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2, ensure_ascii=False)
        print(f"Wrote detected config to {out} (use --apply to overwrite {CONFIG})")

    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
