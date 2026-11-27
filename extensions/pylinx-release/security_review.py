#!/usr/bin/env python3
"""Inspect candidate Git objects without checking out or running their code."""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile

from pipeline import report

CORE_REPO = "https://github.com/MaaAssistantArknights/MaaAssistantArknights.git"
FORK_CONTROL = (".github/", "extensions/pylinx-release/", "extensions/clue-reciprocity/")
APP_EXECUTION = ("gradle/wrapper/", "scripts/", "app/src/main/jniLibs/")
CORE_EXECUTION = ("cmake/", "tools/", "3rdparty/")
CONTROL_FILES = {".gitmodules", ".gitattributes", "gradlew", "gradlew.bat", "CMakeLists.txt", "CMakePresets.json"}
OPAQUE_CODE = {".aar", ".apk", ".class", ".dex", ".dll", ".exe", ".jar", ".so", ".a", ".dylib"}
RULES = {
    "download piped to an interpreter": re.compile(r"\b(?:curl|wget|Invoke-WebRequest)\b[^\n]*\|\s*(?:sudo\s+)?(?:bash|sh|zsh|python\d*|pwsh|iex)\b", re.I),
    "disabled TLS verification": re.compile(r"(?:GIT_SSL_NO_VERIFY\s*[:=]\s*['\"]?(?:1|true)|verify\s*=\s*False|_create_unverified_context|CURLOPT_SSL_VERIFYPEER\s*,\s*(?:0|false)|\bcurl\b[^\n]*\s(?:--insecure|-k)(?:\s|$))", re.I),
    "dynamic script execution": re.compile(r"\b(?:Invoke-Expression\s|eval\s*\(\s*(?:await\s+)?(?:fetch|base64|atob|input)\b|exec\s*\(\s*(?:base64|requests|urllib)\b)", re.I),
    "cleartext Android traffic": re.compile(r"\bandroid:usesCleartextTraffic\s*=\s*['\"]true['\"]|\bcleartextTrafficPermitted\s*=\s*['\"]true['\"]", re.I),
    "unexpected credential or package-install capability": re.compile(r"\bandroid\.permission\.(?:READ_SMS|RECEIVE_SMS|INSTALL_PACKAGES|MANAGE_CREDENTIALS|MANAGE_DEVICE_POLICY_[A-Z_]+)\b"),
}


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def tree_changes(repo, base, head):
    records = git(repo, "diff", "--raw", "--no-abbrev", "--no-renames", "-z", base, head).split(b"\0")
    for metadata, name in zip(records[0::2], records[1::2]):
        old_mode, mode, _, object_id, change = metadata.removeprefix(b":").split()
        yield name.decode("utf-8"), old_mode, mode, object_id, change


def added_lines(repo, base, head, path):
    patch = git(repo, "--literal-pathspecs", "diff", "--no-ext-diff", "--no-textconv",
                "--no-renames", "--unified=0", base, head, "--", path)
    _, first_hunk, hunks = patch.partition(b"\n@@ ")
    return [line[1:] for line in hunks.splitlines() if first_hunk and line.startswith(b"+")]


def scan(repo, base, head, *, core=False):
    findings = []
    scan_input = []
    prefix = "MaaCore" if core else "MAA-Meow"
    sensitive = CORE_EXECUTION if core else APP_EXECUTION + FORK_CONTROL
    for path, old_mode, mode, object_id, change in tree_changes(repo, base, head):
        location = f"{prefix}/{path}"
        if path.startswith(sensitive) or path in CONTROL_FILES or path.endswith((".gradle", ".gradle.kts")):
            findings.append((location, "changes to build or release controls"))
        if change == b"D":
            continue
        if mode in {b"120000", b"160000"} or (mode == b"100755" and old_mode != mode):
            findings.append((location, "new or modified symlink, submodule or executable"))
        if PurePosixPath(path).suffix.lower() in OPAQUE_CODE:
            findings.append((location, "opaque executable artifact"))
        if mode in {b"100644", b"100755"}:
            with subprocess.Popen(["git", "-C", str(repo), "cat-file", "blob", object_id.decode()],
                                  stdout=subprocess.PIPE) as blob:
                magic = blob.stdout.read(4)
                blob.stdout.close()
            if magic.startswith((b"\x7fELF", b"MZ")):
                findings.append((location, "executable binary content"))
        lines = added_lines(repo, base, head, path)
        scan_input.extend(lines)
        for label, pattern in RULES.items():
            if any(pattern.search(line.decode("utf-8", errors="replace")) for line in lines):
                findings.append((location, label))
    return findings, scan_input


def fetch_core(repo, tag, ref):
    git(repo, "fetch", "--no-tags", "--depth=1", CORE_REPO, f"+refs/tags/{tag}:{ref}")
    return git(repo, "rev-parse", f"{ref}^{{commit}}").decode().strip()


def review(args):
    with tempfile.TemporaryDirectory() as temp:
        if args.plan:
            plan = json.loads(args.plan.read_text())
            base, head = plan["baseCommit"], plan["sourceCommit"]
        else:
            head = args.head
            base = git(".", "merge-base", "HEAD", head).decode().strip()
        findings, scan_input = scan(".", base, head)

        if args.plan:
            core_repo = Path(temp) / "core"
            subprocess.run(["git", "init", "--bare", "--quiet", str(core_repo)], check=True)
            plan["coreCommit"] = fetch_core(core_repo, plan["core"], "refs/pylinx/proposed")
            if plan.get("previousCoreCommit") and plan["previousCore"] == plan["core"]:
                if plan["previousCoreCommit"] != plan["coreCommit"]:
                    findings.append(("MaaCore", "an already published upstream tag changed commit"))
            if plan["previousCore"] != plan["core"]:
                previous = fetch_core(core_repo, plan["previousCore"], "refs/pylinx/previous")
                if plan.get("previousCoreCommit") and previous != plan["previousCoreCommit"]:
                    findings.append(("MaaCore", "previously published upstream tag changed commit"))
                core_findings, core_input = scan(core_repo, previous, plan["coreCommit"], core=True)
                findings.extend(core_findings)
                scan_input.extend(core_input)

        # Gitleaks runs with its built-in rules and without candidate-controlled ignore files.
        leaks = subprocess.run(
            [str(args.gitleaks), "stdin", "--no-banner", "--redact=100", "--ignore-gitleaks-allow",
             "--exit-code=42"], cwd=temp, input=b"\n".join(scan_input),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode
        if leaks != 0:
            findings.append(("source diff", "possible committed secret" if leaks == 42 else "credential scanner failed"))
        for path, problem in findings:
            report("error", f"Automatic code review: {problem} in {path!r}")
        if findings:
            return 1
        if args.plan:
            args.plan.write_text(json.dumps(plan, indent=2) + "\n")
        print(f"Automatic code review passed for {head}")
        return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--plan", type=Path)
    mode.add_argument("--head")
    parser.add_argument("--gitleaks", type=Path, required=True)
    raise SystemExit(review(parser.parse_args()))
