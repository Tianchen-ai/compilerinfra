"""Private source snapshots and incremental builds for a compiler scope."""

import json
import os
import shutil
from pathlib import Path

from .process import execute


def relative(value):
    path = Path(value)
    if path.is_absolute() or ".." in path.parts or path == Path("."):
        raise ValueError(f"expected product-relative path, got {value}")
    return path


class Sources:
    def __init__(self, workspace, root, build):
        self.workspace = workspace
        self.source = root / "src"
        self.build = root / "build"
        self.config = build
        self.configure_args = None
        self.outputs = root.parent
        self.initial = None

    async def git(self, *args, cwd=None, data=None):
        result, _ = await execute(["git", *args], cwd or self.source, data=data)
        return result

    async def start(self):
        if self.initial is not None:
            return self.initial
        top = await self.git("rev-parse", "--show-toplevel", cwd=self.workspace)
        if Path(os.fsdecode(top.strip())).resolve() != self.workspace:
            raise ValueError(
                "source derivation needs the product Git root as workspace"
            )
        files = await self.git(
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            cwd=self.workspace,
        )
        self.source.mkdir(exist_ok=True)
        # Take the actual working tree, including uncommitted and untracked sources.
        for entry in set(files.split(b"\0")) - {b""}:
            path = relative(os.fsdecode(entry))
            src, dst = self.workspace / path, self.source / path
            if src.is_relative_to(self.source.parent) or (
                self.outputs != self.workspace
                and self.outputs.is_relative_to(self.workspace)
                and src.is_relative_to(self.outputs)
            ):
                continue
            if src.is_symlink():
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.symlink_to(os.readlink(src))
            elif src.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
            elif src.is_dir():
                raise ValueError(
                    f"nested Git repository {path} must be provided as a build dependency"
                )
        await self.git("init", "-q")
        await self.git("add", "-f", "-A")
        self.initial = (await self.git("write-tree")).decode().strip()
        return self.initial

    async def checkout(self, tree):
        await self.git("read-tree", "--reset", "-u", tree)
        await self.git("clean", "-fdx")

    async def export(self, tree, dest):
        await self.checkout(tree)
        dest.mkdir()
        await self.git("checkout-index", "--all", "--prefix", str(dest) + "/")

    async def change(self, tree, patch):
        await self.checkout(tree)
        if patch:
            await self.git(
                "apply",
                "--index",
                "--binary",
                "--whitespace=nowarn",
                "-",
                data=patch.encode(),
            )
        return (await self.git("write-tree")).decode().strip()

    async def compile(self, tree, install, conf):
        await self.checkout(tree)
        args = tuple(conf.get("configure_args", ()))
        if self.configure_args is not None and args != self.configure_args:
            shutil.rmtree(self.build, ignore_errors=True)
        self.configure_args = args
        await execute(
            [
                "cmake",
                "-S",
                self.source,
                "-B",
                self.build,
                *args,
            ],
            self.source,
        )
        await execute(
            ["cmake", "--build", self.build, "--target", conf["target"]], self.source
        )
        cmd = ["cmake", "--install", self.build, "--prefix", install]
        if "component" in conf:
            cmd += ["--component", conf["component"]]
        await execute(cmd, self.source)
        driver = (install / relative(conf["driver"])).resolve(strict=True)
        if not driver.is_relative_to(install) or not os.access(driver, os.X_OK):
            raise ValueError(
                "candidate driver must be executable inside its installation"
            )
        return driver

    async def adopt(self, tree, preset, ancestor, build):
        import fcntl

        git_dir = await self.git("rev-parse", "--absolute-git-dir", cwd=self.workspace)
        guard = os.open(os.fsdecode(git_dir.rstrip(b"\n")), os.O_RDONLY)
        try:
            try:
                fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as err:
                raise ValueError(
                    "another compiler scope is adopting into this worktree"
                ) from err
            return await self._apply(tree, preset, ancestor, build)
        finally:
            os.close(guard)

    async def _apply(self, tree, preset, ancestor, build):
        await self.checkout(tree)
        if preset:
            if "preset" not in build:
                raise ValueError(
                    "product must declare the source preset used by its normal build"
                )
            file = (self.source / relative(build["preset"])).resolve()
            if not file.is_relative_to(self.source):
                raise ValueError("preset path leaves candidate source")
            current = json.loads(file.read_text()) if file.exists() else {}
            merge(current, preset)
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(json.dumps(current, indent=2) + "\n")
            await self.git("add", "-f", "--", str(file.relative_to(self.source)))
        changed = await self.git(
            "diff", "--name-only", "--no-renames", "-z", "--cached", ancestor
        )
        for raw in changed.split(b"\0"):
            if not raw:
                continue
            path = relative(os.fsdecode(raw))
            dest = self.workspace / path
            if not dest.parent.resolve().is_relative_to(self.workspace):
                raise ValueError(f"adoption crosses a symlink: {path}")
            old = await self.git("ls-tree", "-z", ancestor, "--", str(path))
            if not old:
                matches = not os.path.lexists(dest)
            else:
                mode = old.split(b" ", 1)[0]
                original = await self.git("cat-file", "blob", f"{ancestor}:{path}")
                matches = (
                    dest.is_symlink() and os.fsencode(os.readlink(dest)) == original
                    if mode == b"120000"
                    else dest.is_file()
                    and not dest.is_symlink()
                    and dest.read_bytes() == original
                    and (mode == b"100755") == bool(dest.stat().st_mode & 0o100)
                )
            if not matches:
                raise ValueError(f"adoption conflict in product worktree: {path}")
        patch = await self.git(
            "diff", "--binary", "--no-renames", "--cached", ancestor, "--"
        )
        if patch:
            # git apply rejects a conflicting hunk atomically and leaves the index alone.
            await self.git(
                "apply",
                "--binary",
                "--whitespace=nowarn",
                "-",
                cwd=self.workspace,
                data=patch,
            )
        return (await self.git("write-tree")).decode().strip(), bool(patch)


def merge(base, delta):
    """A family selects a whole chain and realizer; product option values are atomic."""
    for target, fields in delta.items():
        dest = base.setdefault(target, {})
        for key, value in fields.items():
            if key in ("decisions", "optimizations", "options"):
                dest.setdefault(key, {}).update(value)
            else:
                dest[key] = value
    return base
