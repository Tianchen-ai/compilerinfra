"""Publish only this product's files, retaining mapped library inodes and unrelated installs."""

from contextlib import ExitStack, contextmanager
import fcntl
import logging
import json
import os
from pathlib import Path
import shutil
import tempfile
from .tree import relative


@contextmanager
def publish(stage, prefix, backup, library, previous_manifest=None, *, keep_install=None):
    stage, prefix = Path(stage), Path(prefix)
    prefix.mkdir(parents=True, exist_ok=True)
    lock = os.open(prefix, os.O_RDONLY)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if keep_install is not None:
            inventory = prefix / ".compilerinfra" / (library.name + ".files.json")
            for entry in json.loads(inventory.read_text()):
                rel = relative(entry)
                saved = keep_install / rel
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(prefix / rel, saved)
        with ExitStack() as backups:
            with _publish(stage, prefix, backups, library, previous_manifest):
                yield
    finally:
        os.close(lock)


@contextmanager
def _publish(stage, prefix, backups, library, previous_manifest):
    """Replace installed files atomically and restore them if activation fails."""
    done = []
    generation = None
    previous_generation = None
    directories = {}

    def save(dest):
        # A rollback must not need to allocate or copy file contents after ENOSPC.
        # Save on the destination filesystem, including for mounted subdirectories.
        if dest.parent not in directories:
            directories[dest.parent] = Path(backups.enter_context(
                tempfile.TemporaryDirectory(prefix=".ci-rollback-", dir=dest.parent)))
        old = directories[dest.parent] / dest.name
        shutil.copy2(dest, old, follow_symlinks=False)
        return old

    try:
        inventory = Path(".compilerinfra") / (library.name + ".files.json")
        record = prefix / inventory
        old_files = set(json.loads(record.read_text())) if record.exists() else set()
        if (
            not record.exists()
            and (prefix / library).exists()
            and previous_manifest
            and previous_manifest.is_file()
        ):
            paths = [Path(s) for s in previous_manifest.read_text().splitlines()]
            if prefix / library in paths:
                product = library.name.removeprefix("lib").split(".so")[0]
                roots = (
                    prefix / library.parent / (product + ".assets"),
                    prefix / "include" / product,
                )
                old_files.update(
                    str(p.relative_to(prefix))
                    for p in paths
                    if any(p.is_relative_to(r) for r in roots)
                )
        new_files = {str(p.relative_to(stage)) for p in stage.rglob("*") if p.is_file()}
        for other in record.parent.glob("*.files.json"):
            if other != record:
                owned = set(json.loads(other.read_text()))
                conflict = owned & new_files
                if conflict:
                    raise ValueError(
                        f"installation paths owned by {other.name}: {sorted(conflict)}"
                    )
                old_files -= owned
        for removed in sorted(old_files - new_files):
            rel = relative(removed)
            dest = prefix / rel
            if dest.is_file() or dest.is_symlink():
                old = save(dest)
                dest.unlink()
                done.append((dest, old))
        (stage / inventory).parent.mkdir(exist_ok=True)
        (stage / inventory).write_text(json.dumps(sorted(new_files)))
        for src in sorted(stage.rglob("*")):
            if not src.is_file():
                continue
            rel = src.relative_to(stage)
            dest = prefix / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            old = save(dest) if dest.exists() or dest.is_symlink() else None

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
                done.append((dest, old))
            finally:
                path.unlink(missing_ok=True)
        yield
    except BaseException:
        for dest, old in reversed(done):
            if old:
                os.replace(old, dest)
            else:
                dest.unlink()
        if generation:
            generation.unlink(missing_ok=True)
        raise
    if previous_generation:
        # Publication and activation have committed. Reclamation can be
        # retried without reversing the product's source or installed ABI.
        try:
            previous_generation.unlink(missing_ok=True)
        except OSError as error:
            logging.getLogger(__name__).warning("retained %s: %s", previous_generation, error)
