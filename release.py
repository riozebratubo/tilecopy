#!/usr/bin/env python3
"""
release.py — Create a GitHub Release for tilecopy.

Requirements:
  - Python 3.8+
  - gh CLI installed and authenticated (gh auth login)
  - build.bat dependencies (Visual Studio / CMake)
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).parent.resolve()
CMAKE_FILE = REPO_ROOT / "CMakeLists.txt"
ARTIFACT = REPO_ROOT / "build" / "Release" / "tilecopy.exe"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run(cmd, *, capture=True, stream=False, cwd=None):
    """Run a command and return stdout. Raises on non-zero exit."""
    if stream:
        result = subprocess.run(cmd, cwd=cwd or REPO_ROOT, shell=True)
        if result.returncode != 0:
            raise RuntimeError(f"Command failed: {cmd}")
        return ""
    result = subprocess.run(
        cmd, cwd=cwd or REPO_ROOT, shell=True,
        capture_output=capture, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Command failed: {cmd}\n{result.stderr.strip()}"
        )
    return result.stdout.strip()


def ask(prompt, default=None):
    """Prompt the user for input, with an optional pre-filled default."""
    if default:
        display = f"{prompt} [{default}]: "
    else:
        display = f"{prompt}: "
    try:
        value = input(display).strip()
    except (KeyboardInterrupt, EOFError):
        print("\nAborted.")
        sys.exit(1)
    return value if value else (default or "")


def ask_yn(prompt, default_yes=False):
    """Prompt a yes/no question. Returns bool."""
    hint = "Y/n" if default_yes else "y/N"
    try:
        answer = input(f"{prompt} [{hint}]: ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        print("\nAborted.")
        sys.exit(1)
    if not answer:
        return default_yes
    return answer in ("y", "yes")


def separator(char="─", width=60):
    print(char * width)


# ---------------------------------------------------------------------------
# Step 1 — Check gh CLI
# ---------------------------------------------------------------------------

def check_gh():
    print("Checking gh CLI authentication…")
    try:
        run("gh auth status")
    except RuntimeError as e:
        print(f"\nERROR: {e}")
        print("Run 'gh auth login' first.")
        sys.exit(1)
    print("  ✓ gh CLI authenticated.\n")


# ---------------------------------------------------------------------------
# Step 2 — Detect git state
# ---------------------------------------------------------------------------

def get_git_info():
    branch = run("git rev-parse --abbrev-ref HEAD")
    commit = run("git rev-parse HEAD")
    short = commit[:8]
    print(f"Current branch : {branch}")
    print(f"HEAD commit    : {short}\n")
    return branch, commit


# ---------------------------------------------------------------------------
# Step 3 — Read version from CMakeLists.txt
# ---------------------------------------------------------------------------

def read_cmake_version():
    text = CMAKE_FILE.read_text(encoding="utf-8")
    m = re.search(r'project\s*\(\s*\w+\s+VERSION\s+([\d.]+)', text)
    if not m:
        print("WARNING: Could not find VERSION in CMakeLists.txt.")
        return None
    return m.group(1)


# ---------------------------------------------------------------------------
# Step 4 — Get git log since last release tag
# ---------------------------------------------------------------------------

def get_log_since_last_tag():
    try:
        last_tag = run("git describe --tags --abbrev=0")
    except RuntimeError:
        last_tag = None

    if last_tag:
        print(f"  Last release tag: {last_tag}")
        log = run(f"git --no-pager log {last_tag}..HEAD --oneline --no-decorate")
    else:
        print("  No previous release tag found — using full history.")
        log = run("git --no-pager log --oneline --no-decorate")

    return log


# ---------------------------------------------------------------------------
# Step 5 — Open editor for release notes
# ---------------------------------------------------------------------------

def edit_release_notes(prefill: str) -> str:
    editor = os.environ.get("EDITOR") or os.environ.get("VISUAL") or "notepad"

    header = (
        "# Release notes for tilecopy\n"
        "# Lines starting with '#' will be stripped.\n"
        "# Commits since last release:\n"
        "#\n"
    )
    commit_comments = "\n".join(f"# {line}" for line in prefill.splitlines()) if prefill else "# (no commits found)"
    initial = header + commit_comments + "\n\n"

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", prefix="tilecopy_release_",
        delete=False, encoding="utf-8"
    ) as f:
        f.write(initial)
        tmp_path = f.name

    print(f"  Opening editor ({editor})…")
    subprocess.run([editor, tmp_path], check=False)

    notes = Path(tmp_path).read_text(encoding="utf-8")
    os.unlink(tmp_path)

    # Strip comment lines
    cleaned = "\n".join(
        line for line in notes.splitlines()
        if not line.startswith("#")
    ).strip()
    return cleaned


# ---------------------------------------------------------------------------
# Step 6 — Build
# ---------------------------------------------------------------------------

def build():
    print("\nBuilding tilecopy…")
    separator()
    bat = REPO_ROOT / "build.bat"
    subprocess.run(
        str(bat),
        cwd=REPO_ROOT,
        shell=True,
        check=True,
    )
    separator()

    if not ARTIFACT.exists():
        print(f"\nERROR: Build artifact not found: {ARTIFACT}")
        sys.exit(1)
    print(f"\n  ✓ Artifact: {ARTIFACT}\n")


# ---------------------------------------------------------------------------
# Step 7 — Create GitHub Release
# ---------------------------------------------------------------------------

def create_release(*, tag, title, notes, target_commit, prerelease, draft):
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", prefix="tilecopy_notes_",
        delete=False, encoding="utf-8"
    ) as f:
        f.write(notes)
        notes_file = f.name

    try:
        cmd_parts = [
            "gh", "release", "create", tag,
            str(ARTIFACT),
            "--title", title,
            "--notes-file", notes_file,
            "--target", target_commit,
        ]
        if prerelease:
            cmd_parts.append("--prerelease")
        if draft:
            cmd_parts.append("--draft")

        print("Creating GitHub Release…")
        result = subprocess.run(cmd_parts, cwd=REPO_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"\nERROR creating release:\n{result.stderr.strip()}")
            sys.exit(1)

        url = result.stdout.strip()
        return url
    finally:
        os.unlink(notes_file)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    separator("═")
    print("  tilecopy — GitHub Release Creator")
    separator("═")
    print()

    # Step 1
    check_gh()

    # Step 2
    branch, commit = get_git_info()

    # Step 3
    cmake_version = read_cmake_version()
    if cmake_version:
        print(f"CMakeLists version: {cmake_version}\n")
    default_tag = f"v{cmake_version}" if cmake_version else ""

    separator()

    # Prompt 1: tag
    print("1. Release tag")
    tag = ask("   Tag", default=default_tag)
    if not tag:
        print("ERROR: Tag is required.")
        sys.exit(1)
    # Warn on version mismatch
    tag_version = tag.lstrip("v")
    if cmake_version and tag_version != cmake_version:
        print(
            f"\n  ⚠  WARNING: Tag version '{tag_version}' does not match "
            f"CMakeLists.txt version '{cmake_version}'."
        )
        if not ask_yn("   Continue anyway?", default_yes=False):
            print("Aborted.")
            sys.exit(0)
    print()

    # Prompt 2: title
    print("2. Release title")
    default_title = f"tilecopy {tag_version}"
    title = ask("   Title", default=default_title)
    if not title:
        print("ERROR: Title is required.")
        sys.exit(1)
    print()

    # Prompt 3: release notes (editor)
    print("3. Release notes")
    log = get_log_since_last_tag()
    notes = edit_release_notes(log)
    if not notes:
        print("  (empty release notes)")
    print()

    # Prompt 4: pre-release
    print("4. Pre-release")
    prerelease = ask_yn("   Mark as pre-release?", default_yes=False)
    print()

    # Prompt 5: draft
    print("5. Draft")
    draft = ask_yn("   Save as draft (publish later)?", default_yes=False)
    print()

    # Confirm
    separator()
    print("Summary:")
    print(f"  Tag         : {tag}")
    print(f"  Title       : {title}")
    print(f"  Target      : {branch} @ {commit[:8]}")
    print(f"  Pre-release : {'yes' if prerelease else 'no'}")
    print(f"  Draft       : {'yes' if draft else 'no'}")
    print(f"  Notes       : {len(notes)} chars")
    separator()
    print()

    if not ask_yn("Proceed with build and release?", default_yes=True):
        print("Aborted.")
        sys.exit(0)

    # Build
    build()

    # Create release
    url = create_release(
        tag=tag,
        title=title,
        notes=notes,
        target_commit=commit,
        prerelease=prerelease,
        draft=draft,
    )

    separator("═")
    print(f"  ✓ Release created successfully!")
    if url:
        print(f"  {url}")
    separator("═")


if __name__ == "__main__":
    main()
