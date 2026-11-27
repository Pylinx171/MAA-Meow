#!/usr/bin/env python3
"""Prepare, verify and publish the Pylinx171 Android releases."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from urllib.parse import quote
from zipfile import ZIP_DEFLATED, ZipFile

FORK = "Pylinx171/MAA-Meow"
UPSTREAM = "https://github.com/Aliothmoon/MAA-Meow.git"
APP_BRANCH = "pylinx171"
UPDATE_BRANCH = "pylinx-updates"
CANDIDATE = "pylinx-release-candidate"
APP_ID = "com.aliothmoon.maameow.pylinx171"
CORE_TAG = r"v\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?"
# The installed side-by-side app's public signing certificate.
CERT_SHA256 = "7e70e6cf65424494f8dcbf8a37b51f20634ceceb32b7c1a134123af3394a0c11"
APP_HOOKS = {
    "app/src/main/java/com/aliothmoon/maameow/" + path
    for path in (
        "presentation/view/panel/ConfigurationPanel.kt",
        "presentation/view/panel/InfrastConfigPanel.kt",
        "domain/usecase/AnalyzeTaskChainUseCase.kt",
    )
}
CORE_HOOKS = (
    "src/MaaCore/Task/Infrast/InfrastReceptionTask.cpp",
    "src/MaaCore/Task/Infrast/InfrastReceptionTask.h",
    "src/MaaCore/Task/Interface/InfrastTask.cpp",
)


def run(*args, capture=True, **kwargs):
    return subprocess.run(
        args, check=True, text=True, stdout=subprocess.PIPE if capture else None, **kwargs
    ).stdout


def git(*args, **kwargs):
    return (run("git", *args, **kwargs) or "").strip()


def report(level, message):
    escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"::{level}::{escaped}", flush=True)
    if path := os.environ.get("PYLINX_ALERT_FILE"):
        with Path(path).open("a", encoding="utf-8") as file:
            file.write(f"{level.upper()}  {message}\n")


def remote_refs(remote, *patterns):
    lines = git("ls-remote", "--refs", remote, *patterns).splitlines()
    return {ref: sha for sha, ref in (line.split() for line in lines)}


def latest_stable(refs):
    tags = [
        ref.removeprefix("refs/tags/")
        for ref in refs
        if re.fullmatch(r"refs/tags/v\d+\.\d+\.\d+", ref)
    ]
    if not tags:
        raise ValueError("No stable upstream MaaMeow tags found")
    return max(tags, key=lambda tag: tuple(map(int, tag[1:].split("."))))


def core_for_commit(commit):
    subjects = git("log", "--format=%s", "-n", "500", commit)
    match = re.search(r"^(?:chore|feat)[：:]\s*适配\s*(?:MAA|上游)\s*(" + CORE_TAG + r")\s*$", subjects, re.M)
    if not match:
        raise ValueError("Pinned MaaCore version could not be inferred from upstream history. Set the maa_core input.")
    return match[1]


def read_index(remote):
    ref = f"refs/heads/{UPDATE_BRANCH}"
    if ref not in remote_refs(remote, ref):
        return None
    git("fetch", "--no-tags", remote, f"+{ref}:refs/pylinx/update-index")
    return json.loads(git("show", "refs/pylinx/update-index:update.json"))


def output(name, value):
    if file := os.environ.get("GITHUB_OUTPUT"):
        with Path(file).open("a", encoding="utf-8") as stream:
            stream.write(f"{name}={value}\n")


def prepare(args):
    index = read_index(args.fork)
    base = git("rev-parse", "HEAD")
    stable = args.upstream_tag or latest_stable(remote_refs(args.upstream, "refs/tags/v*"))
    if not re.fullmatch(r"v\d+\.\d+\.\d+", stable):
        raise ValueError("Upstream MaaMeow tag must be a stable tag such as v0.28.2")
    git("fetch", "--no-tags", args.upstream, f"+refs/tags/{stable}:refs/pylinx/maameow")
    upstream = git("rev-parse", "refs/pylinx/maameow^{commit}")
    core = args.maa_core or core_for_commit(upstream)
    if not re.fullmatch(CORE_TAG, core):
        raise ValueError("Invalid MaaCore tag")

    if index:
        if index["upstreamTag"] == stable and index["upstreamCommit"] != upstream:
            raise ValueError(f"Upstream tag {stable} moved after the last release")
        if tuple(map(int, stable[1:].split("."))) < tuple(map(int, index["upstreamTag"][1:].split("."))):
            raise ValueError("Requested MaaMeow tag predates the published upstream version")
        if not args.force and not args.maa_core and (
            index["sourceCommit"] == base and index["upstreamCommit"] == upstream
        ):
            output("build", "false")
            print(f"{stable} is already published for this source")
            return

    merge_base = git("merge-base", base, upstream)
    upstream_paths = set(git("diff", "--name-only", merge_base, upstream).splitlines())
    local_paths = set(git("diff", "--name-only", merge_base, base).splitlines())
    for path in sorted(upstream_paths & (local_paths | APP_HOOKS)):
        report("warning", f"Upstream {stable} ({upstream[:12]}) changed an integration file: {path}")
    workflow_paths = sorted(path for path in upstream_paths if path.startswith(".github/workflows/"))
    if workflow_paths:
        report("warning", "Upstream workflow changes require review; kept fork workflows: " + ", ".join(workflow_paths))

    git("checkout", "-b", CANDIDATE)
    author = ("-c", "user.name=Pylinx release bot", "-c", "user.email=actions@users.noreply.github.com")
    try:
        git(*author, "merge", "--no-edit", "--no-ff", "--no-commit", upstream, capture=False)
    except subprocess.CalledProcessError:
        for path in git("diff", "--name-only", "--diff-filter=U").splitlines():
            report("error", f"Merge conflict with upstream {stable}: {path}")
        raise
    if subprocess.run(["git", "rev-parse", "--verify", "--quiet", "MERGE_HEAD"],
                      stdout=subprocess.DEVNULL).returncode == 0:
        if workflow_paths:
            git("restore", "--source", base, "--staged", "--worktree", ":(top).github/workflows/")
        git(*author, "commit", "-m", f"chore(upstream): 合并 MAA-Meow {stable}", capture=False)

    tags = remote_refs(args.fork, "refs/tags/pylinx-v*")
    codes = [
        int(match[1]) for tag in tags
        if (match := re.fullmatch(r"refs/tags/pylinx-v\d+\.\d+\.\d+-(\d+)", tag))
    ]
    previous_code = index["app"]["versionCode"] if index else 0
    code = max([1010, previous_code, *codes]) + 1
    version_name = stable[1:]
    plan = {
        "baseCommit": base,
        "sourceCommit": git("rev-parse", "HEAD"),
        "upstreamCommit": upstream,
        "upstreamTag": stable,
        "core": core,
        "previousCore": index["resource"]["core"] if index else "v6.23.1",
        "previousCoreCommit": index["resource"].get("coreCommit") if index else None,
        "versionName": version_name,
        "versionCode": code,
        "tag": f"pylinx-{stable}-{code}",
    }
    args.plan.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    output("build", "true")
    output("core", core)
    output("version_name", version_name)
    output("version_code", code)
    print(f"Prepared {stable} / MaaCore {core} / Android versionCode {code}")


def core_warnings(args):
    plan = json.loads(args.plan.read_text())
    previous = plan["previousCore"]
    core = plan["core"]
    if previous == core:
        return
    run("git", "-C", str(args.core_tree), "fetch", "--no-tags", "--depth", "1", "--no-recurse-submodules", "origin",
        f"+refs/tags/{previous}:refs/pylinx/previous", capture=False)
    paths = run("git", "-C", str(args.core_tree), "diff", "--name-only", "refs/pylinx/previous", "HEAD", "--", *CORE_HOOKS)
    for path in paths.splitlines():
        report("warning", f"MaaCore {previous} → {core} changed an integration file: {path}")


def sha256(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def apk_identity(apk, build_tools):
    badging = run(str(build_tools / "aapt"), "dump", "badging", str(apk))
    match = re.search(r"^package: name='([^']+)' versionCode='(\d+)' versionName='([^']+)'", badging, re.M)
    if not match:
        raise ValueError("Cannot read APK package and version")
    return match[1], int(match[2]), match[3]


def check_apk(apk, build_tools, plan):
    if apk_identity(apk, build_tools) != (APP_ID, plan["versionCode"], plan["versionName"]):
        raise ValueError("APK package or Android version does not match release plan")
    with ZipFile(apk) as archive:
        libs = [name for name in archive.namelist() if re.fullmatch(r"lib/[^/]+/libMaaCore\.so", name)]
        if libs != ["lib/arm64-v8a/libMaaCore.so"]:
            raise ValueError(f"APK native ABI is incorrect: {libs}")
        lib = archive.read(libs[0])
        if lib[:6] != b"\x7fELF\x02\x01" or int.from_bytes(lib[18:20], "little") != 183 or b"PylinxClueReciprocity" not in lib:
            raise ValueError("APK does not contain the complete ARM64 custom MaaCore")
        if "assets/pylinx/clue-reciprocity-arm64-v8a" not in archive.namelist():
            raise ValueError("APK is missing the native extension availability marker")


def package(args):
    args.out.mkdir(parents=True, exist_ok=True)
    plan = json.loads(args.plan.read_text())
    check_apk(args.apk, args.build_tools, plan)
    version = json.loads((args.resource / "version.json").read_text())["last_updated"]
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}", version):
        raise ValueError(f"Invalid MaaResource timestamp: {version}")
    for entry in ("version.json", "config.json", "tasks/tasks.json"):
        if not (args.resource / entry).is_file():
            raise ValueError(f"Resource package is missing {entry}")
    with ZipFile(args.out / "MaaResource.zip", "w", compression=ZIP_DEFLATED, compresslevel=4) as archive:
        for path in sorted(args.resource.rglob("*")):
            if path.is_file():
                archive.write(path, "resource/" + path.relative_to(args.resource).as_posix())
    plan["resourceVersion"] = version
    (args.out / "plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    shutil.copyfile(args.apk, args.out / "app-unsigned.apk")


def make_index(plan, apk, resource):
    tag = plan["tag"]
    base = f"https://github.com/{FORK}/releases/download/{quote(tag)}"

    def asset(file):
        return {"url": f"{base}/{quote(file.name)}", "sha256": sha256(file)}

    return {
        "schema": 1,
        "sourceCommit": plan["sourceCommit"],
        "upstreamCommit": plan["upstreamCommit"],
        "upstreamTag": plan["upstreamTag"],
        "app": {
            "versionName": plan["versionName"],
            "versionCode": plan["versionCode"],
            "abi": "arm64-v8a",
            "asset": asset(apk),
            "releaseNote": f"Upstream MaaMeow {plan['upstreamTag']} · MaaCore {plan['core']} · Pylinx171",
        },
        "resource": {
            "core": plan["core"],
            "coreCommit": plan["coreCommit"],
            "version": plan["resourceVersion"],
            "asset": asset(resource),
        },
    }


def commit_index(index, previous):
    body = json.dumps(index, indent=2, ensure_ascii=False) + "\n"
    blob = git("hash-object", "-w", "--stdin", input=body)
    tree = git("mktree", input=f"100644 blob {blob}\tupdate.json\n")
    parents = ["-p", git("rev-parse", "refs/pylinx/update-index")] if previous else []
    return git("-c", "user.name=Pylinx release bot", "-c", "user.email=actions@users.noreply.github.com",
               "commit-tree", tree, *parents, "-m", f"chore(release): 更新 Android 构建 {index['app']['versionCode']} 的更新清单")


def publish(args):
    plan = json.loads((args.approved / "plan.json").read_text())
    built_plan = json.loads((args.artifacts / "plan.json").read_text())
    resource_version = built_plan.pop("resourceVersion")
    if built_plan != plan or not re.fullmatch(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}", resource_version):
        raise ValueError("Build artifact does not match the reviewed release plan")
    plan["resourceVersion"] = resource_version
    apk = args.apk
    resource = args.artifacts / "MaaResource.zip"
    check_apk(apk, args.build_tools, plan)
    certificates = run(str(args.build_tools / "apksigner"), "verify", "--print-certs", str(apk))
    hashes = re.findall(r"certificate SHA-256 digest: ([a-fA-F0-9]+)", certificates)
    if [h.lower() for h in hashes] != [CERT_SHA256]:
        raise ValueError("APK does not match the installed Pylinx171 signing certificate")

    previous = read_index("origin")
    if previous and previous["app"]["versionCode"] >= plan["versionCode"]:
        raise ValueError("A newer build is already published")
    ref = f"refs/heads/{APP_BRANCH}"
    if remote_refs("origin", ref).get(ref) != plan["baseCommit"]:
        raise ValueError("Fork changed during the build; rerun against its latest commit")
    candidate = plan["baseCommit"]
    if plan["sourceCommit"] != plan["baseCommit"]:
        git("fetch", "--no-tags", str((args.approved / "source.bundle").resolve()),
            f"refs/heads/{CANDIDATE}:refs/pylinx/candidate")
        candidate = git("rev-parse", "refs/pylinx/candidate")
    if candidate != plan["sourceCommit"]:
        raise ValueError("Source bundle does not match the tested build")
    index = make_index(plan, apk, resource)
    index_commit = commit_index(index, previous)
    tag = plan["tag"]
    git("push", "--atomic", "origin", f"{candidate}:{ref}", f"{candidate}:refs/tags/{tag}", capture=False)
    run("gh", "release", "create", tag, str(apk), str(resource), "--repo", FORK, "--verify-tag",
        "--draft", "--title", f"Pylinx171 {plan['versionName']} ({plan['versionCode']})",
        "--notes", index["app"]["releaseNote"], capture=False)
    release = json.loads(run(
        "gh", "api", f"repos/{FORK}/releases?per_page=100", "--paginate",
        "--jq", f".[] | select(.tag_name == {json.dumps(tag)})",
    ))
    if not release["draft"]:
        raise ValueError(f"Expected {tag} to remain a draft during asset verification")
    assets = {asset["name"]: asset for asset in release["assets"]}
    for file, artifact in ((apk, index["app"]["asset"]), (resource, index["resource"]["asset"])):
        found = assets.get(file.name)
        if not found or found["state"] != "uploaded" or found["digest"] != f"sha256:{artifact['sha256']}":
            raise ValueError(f"GitHub did not verify the uploaded asset {file.name}")
    run("gh", "release", "edit", tag, "--repo", FORK, "--draft=false", "--latest", capture=False)
    git("push", "origin", f"{index_commit}:refs/heads/{UPDATE_BRANCH}", capture=False)
    print(f"Published {tag} and updated {UPDATE_BRANCH}/update.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    sub = commands.add_parser("prepare")
    sub.add_argument("--plan", type=Path, required=True)
    sub.add_argument("--upstream", default=UPSTREAM)
    sub.add_argument("--fork", default="origin")
    sub.add_argument("--upstream-tag")
    sub.add_argument("--maa-core")
    sub.add_argument("--force", action="store_true")
    sub.set_defaults(function=prepare)

    sub = commands.add_parser("core-warnings")
    sub.add_argument("--plan", type=Path, required=True)
    sub.add_argument("--core-tree", type=Path, required=True)
    sub.set_defaults(function=core_warnings)

    sub = commands.add_parser("package")
    sub.add_argument("--plan", type=Path, required=True)
    sub.add_argument("--resource", type=Path, required=True)
    sub.add_argument("--apk", type=Path, required=True)
    sub.add_argument("--out", type=Path, required=True)
    sub.add_argument("--build-tools", type=Path, required=True)
    sub.set_defaults(function=package)

    sub = commands.add_parser("publish")
    sub.add_argument("--artifacts", type=Path, required=True)
    sub.add_argument("--approved", type=Path, required=True)
    sub.add_argument("--apk", type=Path, required=True)
    sub.add_argument("--build-tools", type=Path, required=True)
    sub.set_defaults(function=publish)

    args = parser.parse_args()
    try:
        args.function(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        report("error", str(error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
