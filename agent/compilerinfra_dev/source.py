"""Independent product builds and reversible adoption into the working tree."""

from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from compilerinfra.tree import read, relative, write


def command(root, argv, data=None, env=None):
    run = subprocess.run(argv, cwd=root, input=data, capture_output=True, env=env)
    if run.returncode:
        raise ValueError(
            f"{argv[0]} exited {run.returncode}\n"
            + (run.stderr + run.stdout).decode(errors="replace")
        )
    return run.stdout


def git(root, *args, data=None):
    return command(root, ["git", *args], data)


def snapshot(root, dest):
    dest.mkdir(parents=True)
    files = git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
    for item in set(files.split(b"\0")) - {b""}:
        rel = relative(os.fsdecode(item))
        src, to = root / rel, dest / rel
        if not src.exists() and not src.is_symlink():
            continue
        to.parent.mkdir(parents=True, exist_ok=True)
        if src.is_symlink():
            to.symlink_to(os.readlink(src))
        else:
            shutil.copy2(src, to)
    git(dest, "init", "-q")
    head = subprocess.run(
        ["git", "-C", root, "rev-parse", "--verify", "--quiet", "HEAD"],
        capture_output=True,
    )
    if head.returncode == 0:
        revision = head.stdout.decode().strip()
        git(dest, "fetch", "--quiet", "--depth=1", str(root), revision)
        git(dest, "update-ref", "HEAD", revision)
        git(dest, "read-tree", revision)
    git(dest, "add", "-f", "-A")
    return git(dest, "write-tree").decode().strip()


class Snapshot:
    def __init__(self, folder, roots):
        self.roots = {
            k: folder / ("product" if k == "." else "extensions/" + k) for k in roots
        }
        self.initial = {k: snapshot(src, self.roots[k]) for k, src in roots.items()}

    def derive(self, folder, patches):
        child = object.__new__(Snapshot)
        child.initial = self.initial
        child.roots = {}
        for key, src in self.roots.items():
            dest = folder / ("product" if key == "." else "extensions/" + key)
            shutil.copytree(src, dest, symlinks=True)
            child.roots[key] = dest
        for key, patch in patches.items():
            git(child.roots[key], "apply", "--binary", "-", data=patch.encode())
        return child

    def delta(self, configuration):
        patches = {}
        for key, root in self.roots.items():
            if key == ".":
                config = root / relative(read(root / "product.json")["configuration"])
                if read(config) != configuration:
                    write(config, configuration)
            git(root, "add", "-f", "-A")
            patches[key] = git(
                root, "diff", "--cached", "--binary", "--no-renames", self.initial[key]
            )
        return patches


@contextmanager
def adoption_lock(root):
    gitdir = git(root, "rev-parse", "--absolute-git-dir").decode().strip()
    fd = os.open(gitdir, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


def build(roots, directory, install):
    root = roots["."]
    product = read(root / "product.json")
    env = dict(
        os.environ,
        CI_EXTENSION_ROOTS=json.dumps(
            {k: str(v) for k, v in roots.items() if k != "."}
        ),
    )
    command(
        root,
        [
            "cmake",
            "-S",
            root,
            "-B",
            directory,
            "-DCMAKE_INSTALL_PREFIX=" + str(install),
            *product["build"].get("configure_args", []),
        ],
        env=env,
    )
    command(root, ["cmake", "--build", directory], env=env)
    command(root, ["cmake", "--install", directory, "--prefix", install], env=env)
    lib = install / relative(product["install"]["library"])
    return lib.resolve(strict=True)


@contextmanager
def applied(roots, patches):
    done = []
    try:
        for key, patch in patches.items():
            if patch:
                git(roots[key], "apply", "--binary", "-", data=patch)
                done.append(key)
        yield
    except BaseException:
        for key in reversed(done):
            git(roots[key], "apply", "--reverse", "--binary", "-", data=patches[key])
        raise


@contextmanager
def publish(stage, prefix, backup, library):
    """Replace installed files atomically and restore them if activation fails."""
    done = []
    generation = None
    previous_generation = None
    try:
        for src in sorted(stage.rglob("*")):
            if not src.is_file():
                continue
            rel = src.relative_to(stage)
            dest = prefix / rel
            old = backup / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            exists = dest.exists()
            if exists:
                old.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dest, old, follow_symlinks=False)

            with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as tmp:
                path = Path(tmp.name)
            try:
                shutil.copy2(src, path)
                if rel == library:
                    # A previously dlopen'ed pathname can still resolve to the
                    # old mapping in a long-lived JIT. Publish a fresh realpath.
                    generation = path.with_name(f".{dest.name}.build-{path.name}")
                    if dest.is_symlink() and dest.resolve().name.startswith(
                        f".{dest.name}.build-"
                    ):
                        previous_generation = dest.resolve()
                    os.replace(path, generation)
                    path.symlink_to(generation.name)
                os.replace(path, dest)
            finally:
                path.unlink(missing_ok=True)
            done.append((dest, old if exists else None))
        yield
    except BaseException:
        for dest, old in reversed(done):
            if old:
                dest.unlink()
                shutil.copy2(old, dest, follow_symlinks=False)
            else:
                dest.unlink()
        if generation:
            generation.unlink(missing_ok=True)
        raise
    if previous_generation:
        previous_generation.unlink(missing_ok=True)
