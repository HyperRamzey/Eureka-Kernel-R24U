#!/usr/bin/env python3
"""publish_release.py - stage and publish a Eureka kernel release.

The modern (crDroid 16 / ``rom-crdroid16``) kernel is built by the ROM tree:

    source build/envsetup.sh && breakfast lineage_a30s-bp4a-userdebug
    m bootimage          # kernel + boot.img only
    m bacon              # full ROM, also refreshes boot/dtb/dtbo

This script takes the build outputs, packages the AnyKernel3 flashable zip
(template: ``tools_eureka/kernel_zip/anykernel`` or a staged copy), writes
sha256 sidecars, and creates/updates the GitHub release with the usual
kernel-YYYYMMDD tag.

Run it inside WSL with the token at /root/.github_token (or GITHUB_TOKEN set):

    python3 tools_eureka/publish_release.py \
      --tag kernel-20261019 --version 249 --date 2026-10-19 \
      --boot  /root/rom/crdroid16/out/target/product/a30s/boot.img \
      --image /root/rom/crdroid16/out/target/product/a30s/obj/KERNEL_OBJ/arch/arm64/boot/Image \
      --dtb   /root/rom/crdroid16/out/target/product/a30s/dtb.img \
      --dtbo  /root/rom/crdroid16/out/target/product/a30s/dtbo.img \
      --ak-dir /root/rom/crdroid16/kernel/samsung/exynos7885/kernel_zip/anykernel \
      --notes-file NOTES_kernel-20261019.md

The release body is only written when the release is created; pass
--update-body to replace it on an existing tag. Assets already attached are
skipped unless --replace is given.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"
DEFAULT_REPO = "HyperRamzey/Eureka-Kernel-R24U"
DEFAULT_TOKEN_FILE = "/root/.github_token"


def read_token(path):
    env = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if env:
        return env.strip()
    if os.path.isfile(path):
        with open(path) as fh:
            return fh.read().strip()
    sys.exit(f"no token: set GITHUB_TOKEN or create {path}")


class ApiError(RuntimeError):
    def __init__(self, method, url, code, detail):
        super().__init__(f"GitHub API {method} {url}: HTTP {code}\n{detail[:800]}")
        self.code = code


def api(url, token, data=None, method="GET", stream=None, content_type=None):
    headers = {
        "Accept": "application/vnd.github+json",
        "Authorization": "Bearer " + token,
        "User-Agent": "eureka-publish-release",
    }
    if stream is not None:
        body = stream
        headers["Content-Type"] = content_type or "application/octet-stream"
        headers["Content-Length"] = str(os.fstat(stream.fileno()).st_size)
    else:
        body = json.dumps(data).encode() if data is not None else None
        if body is not None:
            headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=1800) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        raise ApiError(method, url, exc.code, exc.read().decode(errors="replace")) from None


def api_or_none(url, token, **kw):
    try:
        return api(url, token, **kw)
    except ApiError as exc:
        if exc.code == 404:
            return None
        raise


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(32 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sidecar(path):
    digest = sha256_file(path)
    sidecar = path + ".sha256"
    with open(sidecar, "w") as fh:
        fh.write(f"{digest}  {os.path.basename(path)}\n")
    return digest


def build_ak3(ak_dir, image, out_zip):
    work = out_zip[:-4] + ".d"
    if os.path.isdir(work):
        shutil.rmtree(work)
    shutil.copytree(ak_dir, work)
    shutil.copy2(image, os.path.join(work, "Image"))
    if os.path.exists(out_zip):
        os.unlink(out_zip)
    subprocess.run(
        ["zip", "-r", "-q", os.path.abspath(out_zip), "."],
        cwd=work, check=True)
    shutil.rmtree(work)
    return out_zip


def main():
    ap = argparse.ArgumentParser(description="Stage and publish a Eureka kernel release")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--tag", required=True, help="release tag, e.g. kernel-20261019")
    ap.add_argument("--name", default=None,
                    help="release title (default: 'Eureka Kernel #<version> (<date>)')")
    ap.add_argument("--version", required=True, help="kernel build number, e.g. 249")
    ap.add_argument("--date", required=True, help="release date, YYYY-MM-DD")
    ap.add_argument("--boot", required=True, help="built boot.img")
    ap.add_argument("--image", required=True, help="built arch/arm64/boot/Image")
    ap.add_argument("--dtb", required=True, help="built dtb.img")
    ap.add_argument("--dtbo", required=True, help="built dtbo.img")
    ap.add_argument("--ak-dir", required=True, help="AnyKernel3 template directory")
    ap.add_argument("--notes-file", default=None, help="release body markdown")
    ap.add_argument("--stage-dir", default=None,
                    help="output directory (default: krel_<YYYYMMDD> next to this script)")
    ap.add_argument("--zip-name", default=None,
                    help="AK3 zip name (default: Eureka-Kernel-a30s-<YYYYMMDD>.zip)")
    ap.add_argument("--token-file", default=DEFAULT_TOKEN_FILE)
    ap.add_argument("--reuse-zip", action="store_true",
                    help="keep an existing staged zip instead of rebuilding it")
    ap.add_argument("--replace", action="store_true", help="re-upload attached assets")
    ap.add_argument("--update-body", action="store_true", help="replace the release body")
    ap.add_argument("--draft", action="store_true")
    ap.add_argument("--prerelease", action="store_true")
    ap.add_argument("--no-upload", action="store_true", help="stage only, do not touch GitHub")
    args = ap.parse_args()

    for path in (args.boot, args.image, args.dtb, args.dtbo, args.ak_dir):
        if not os.path.exists(path):
            sys.exit(f"missing: {path}")
    if args.notes_file and not os.path.isfile(args.notes_file):
        sys.exit(f"missing notes: {args.notes_file}")

    ymd = args.date.replace("-", "")
    stage = args.stage_dir or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                           "krel_" + ymd)
    os.makedirs(stage, exist_ok=True)

    zip_name = args.zip_name or f"Eureka-Kernel-a30s-{ymd}.zip"
    zip_path = os.path.join(stage, zip_name)
    if args.reuse_zip and os.path.isfile(zip_path):
        print(f"reusing {zip_name} (sha256 {sha256_file(zip_path)})")
    else:
        print(f"building {zip_name} from {args.ak_dir} ...")
        build_ak3(args.ak_dir, args.image, zip_path)

    assets = []
    for src, name in ((args.boot, "boot.img"), (args.dtb, "dtb.img"),
                      (args.dtbo, "dtbo.img"), (zip_path, zip_name)):
        dst = os.path.join(stage, name)
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copy2(src, dst)
        digest = write_sidecar(dst)
        assets.append(dst)
        print(f"  {name:40s} {os.path.getsize(dst):>12d} B  sha256 {digest}")
    assets += [a + ".sha256" for a in assets]

    body = ""
    if args.notes_file:
        with open(args.notes_file, encoding="utf-8") as fh:
            body = fh.read()
        with open(os.path.join(stage, "NOTES_FINAL.md"), "w", encoding="utf-8") as fh:
            fh.write(body)

    if args.no_upload:
        print(f"\nstaged in {stage}; --no-upload given, nothing pushed")
        return

    token = read_token(args.token_file)
    title = args.name or f"Eureka Kernel #{args.version} ({args.date})"
    rel = api_or_none(f"{API}/repos/{args.repo}/releases/tags/{args.tag}", token)
    if rel is None:
        rel = api(f"{API}/repos/{args.repo}/releases", token, {
            "tag_name": args.tag,
            "target_commitish": "rom-crdroid16",
            "name": title,
            "body": body,
            "draft": args.draft,
            "prerelease": args.prerelease,
        }, method="POST")
        print(f"\ncreated release {args.tag} (id {rel['id']})")
    else:
        print(f"\nreusing release {args.tag} (id {rel['id']})")
        if args.update_body and args.notes_file:
            rel = api(f"{API}/repos/{args.repo}/releases/{rel['id']}", token,
                      {"body": body, "name": title}, method="PATCH")
            print("  release body updated")

    existing = {a["name"]: a for a in rel.get("assets", [])}
    for path in assets:
        fname = os.path.basename(path)
        if fname in existing and not args.replace:
            print(f"  {fname}: already attached, skipped")
            continue
        if fname in existing:
            api(f"{API}/repos/{args.repo}/releases/assets/{existing[fname]['id']}",
                token, method="DELETE")
        ctype = "application/zip" if fname.endswith(".zip") else "application/octet-stream"
        with open(path, "rb") as fh:
            asset = api(f"{UPLOADS}/repos/{args.repo}/releases/{rel['id']}/assets?name={fname}",
                        token, method="POST", stream=fh, content_type=ctype)
        print(f"  {fname}: uploaded (state {asset.get('state')}, digest {asset.get('digest')})")

    print(f"\nhttps://github.com/{args.repo}/releases/tag/{args.tag}")


if __name__ == "__main__":
    try:
        main()
    except ApiError as exc:
        sys.exit(str(exc))
