"""Installed compiler candidates, each retaining its own build and effective configuration."""

from dataclasses import dataclass
from importlib import import_module
import inspect
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
from uuid import uuid4
from compilerinfra.tree import provenance, relative, tree
from .source import Snapshot, adoption_lock, applied, build, publish
from .worker import Worker


@dataclass
class Candidate:
    worker: Worker
    snapshot: Snapshot | None
    description: dict
    parent: str | None
    configuration: dict
    events: list | None = None
    measured_exit: str | None = None


def overlay(base, delta):
    import copy

    result = copy.deepcopy(base)
    for section, scopes in delta.items():
        for scope, settings in scopes.items():
            current = result.setdefault(section, {}).setdefault(scope, {})
            for group, items in settings.items():
                current.setdefault(group, {}).update(items)
    return result


class Scope:
    def __init__(self, workspace, output_dir, library, profile):
        self.workspace = Path(workspace).resolve()
        self.outputs = Path(output_dir).resolve()
        if (
            library
            and profile == "development"
            and self.outputs.is_relative_to(self.workspace)
        ):
            rel = self.outputs.relative_to(self.workspace) / ".compiler-scope"
            if subprocess.run(
                ["git", "-C", self.workspace, "check-ignore", "-q", str(rel)]
            ).returncode:
                raise ValueError(
                    f"place --output-dir outside product sources or in a Git-ignored directory: {self.outputs}"
                )
        self.outputs.mkdir(parents=True, exist_ok=True)
        self.private = TemporaryDirectory(dir=self.outputs, prefix=".compiler-")
        self.root = Path(self.private.name)
        self.profile = profile
        self.library = Path(library).resolve(strict=True) if library else None
        self.values = {}
        self.owned = {}
        self.base_key = None
        if self.library:
            try:
                self.load_base()
            except BaseException:
                self.close()
                raise

    def load_base(self):
        key = uuid4().hex
        # Pin the library inode and its installed assets for the lifetime of
        # this build. A later adoption can replace the public installation.
        pinned = self.root / key / "baseline" / self.library.parent.name
        pinned.mkdir(parents=True)
        for file in self.library.parent.iterdir():
            if file == self.library:
                shutil.copy2(file, pinned / file.name)
            elif (
                file.name
                == self.library.name.removeprefix("lib").split(".so")[0] + ".assets"
            ):
                shutil.copytree(file, pinned / file.name)
            else:
                (pinned / file.name).symlink_to(file)
        worker = Worker(pinned / self.library.name, self.profile)
        try:
            desc = worker.call("open", key, configuration={})
            snap = None
            if self.profile == "development":
                model = tree(self.workspace)
                self.check_fingerprint(desc, model)
                roots = {k: Path(p) for k, p in model["packages"].items()}
                snap = Snapshot(self.root / key, roots)
                self.check_fingerprint(desc, model)
                worker.call("activate", key)
            self.values[key] = Candidate(worker, snap, desc, None, {})
            self.owned[worker] = self.root / key
            self.base_key = key
        except BaseException:
            worker.close()
            raise

    @staticmethod
    def check_fingerprint(desc, model):
        actual = provenance(model)
        if actual != desc["fingerprint"]:
            raise ValueError(
                f"workspace differs from installation: installed={desc['fingerprint']} workspace={actual}"
            )

    def key(self, id):
        k = self.base_key if id == "base" else id
        if k not in self.values:
            raise ValueError(
                f"unknown compiler {id}; open an installed product with --library"
            )
        return k

    def derive(self, parent, configuration, patch, extensions):
        base_key = self.key(parent)
        base = self.values[base_key]
        key = uuid4().hex
        folder = self.root / key
        changes = overlay(base.configuration, configuration)
        worker, snap = base.worker, base.snapshot
        patches = dict(extensions or {})
        if patch:
            patches["."] = patch
        try:
            if patches:
                snap = snap.derive(folder, patches)
                lib = build(snap.roots, folder / "build", folder / "install")
                worker = Worker(lib, self.profile)
                desc = worker.call("open", key, configuration=changes)
                self.owned[worker] = folder
            else:
                desc = worker.call(
                    "derive", key, parent=base_key, configuration=configuration
                )
            worker.call("activate", key)
            self.values[key] = Candidate(worker, snap, desc, base_key, changes)
        except BaseException:
            if worker is not base.worker:
                worker.close()
            elif not patches:
                # Derivation may have succeeded before an installed action failed to load.
                try:
                    worker.call("discard", key)
                except ValueError:
                    pass
            if folder.exists():
                shutil.rmtree(folder)
            raise
        return key

    def discard(self, id):
        key = self.key(id)
        if key == self.base_key:
            raise ValueError("the active baseline is in use")
        c = self.values[key]
        c.worker.call("discard", key)
        del self.values[key]
        if not any(x.worker is c.worker for x in self.values.values()):
            c.worker.close()
            folder = self.owned.pop(c.worker)
            if folder.exists():
                shutil.rmtree(folder)
        return key

    def adopt(self, id):
        key = self.key(id)
        c = self.values[key]
        folder = self.root / uuid4().hex
        with adoption_lock(self.workspace):
            model = tree(self.workspace)
            self.check_fingerprint(self.values[self.base_key].description, model)
            if c.snapshot.initial != self.values[self.base_key].snapshot.initial:
                raise ValueError(
                    "candidate source diverged from the installed baseline"
                )
            roots = {k: Path(p) for k, p in model["packages"].items()}
            # Write configuration in a disposable snapshot to keep existing candidates immutable.
            snap = c.snapshot.derive(folder, {})
            patches = snap.delta(c.description["configuration"])
            with applied(roots, patches):
                stage = folder / "install"
                lib = build(roots, folder / "build", stage)
                rel = relative(c.description["product"]["install"]["library"])
                prefix = self.library.parents[len(rel.parts) - 1]
                if (prefix / rel) != self.library:
                    raise ValueError(
                        "installed library does not match product install entry"
                    )
                with publish(stage, prefix, folder / "backup"):
                    if lib.relative_to(stage) != rel:
                        raise ValueError(
                            "install did not produce expected compiler library"
                        )
                    previous = self.base_key
                    self.load_base()
            return {
                "base": self.base_key,
                "previous": previous,
                "library": str(self.library),
            }

    def compile(
        self,
        id,
        source,
        exit,
        through=None,
        after=None,
        observe=False,
        author_input=False,
    ):
        key = self.key(id)
        c = self.values[key]
        c.events, c.measured_exit = None, None
        result = c.worker.call(
            "compile",
            key,
            source=source,
            exit=exit,
            through=through,
            after=after,
            text=bool(through),
            explain=observe,
            author_input=author_input,
        )
        if observe:
            artifact, c.events = result
            c.measured_exit = exit
            return artifact
        return result

    def impact(self, id, component):
        c = self.values[self.key(id)]
        item = c.description["components"][component]
        at = item["at"]
        reachable = [
            k for k, v in c.description["exits"].items() if at in v["boundaries"]
        ]
        owner = c.description["units"][item["unit"]]["owner"]
        if owner.startswith("exits/"):
            reachable = [e for e in reachable if owner == "exits/" + e]
        elif owner.startswith("levels/"):
            node = owner.split("/")[-1] + "@"
            reachable = [
                e
                for e in reachable
                if any(
                    b.startswith(node) for b in c.description["exits"][e]["boundaries"]
                )
            ]
        events = c.events
        return dict(
            reachable=reachable,
            selected=[
                e for e in reachable if component in c.description["selected"][e]
            ],
            observed_exit=c.measured_exit,
            hits=None
            if events is None
            else [
                v
                for v in events
                if v["action"] != "no_match"
                and component in (v["implementation"], v["family"])
            ],
        )

    async def measure(self, id, workload, output_dir):
        key = self.key(id)
        c = self.values[key]
        p = c.description["product"]
        adapter = p.get("bench")
        if not adapter:
            raise ValueError("product has no external Bench adapter")
        request = dict(
            candidate=key,
            library=str(c.worker.library),
            configuration=c.description["configuration"],
            python=p["install"]["python"],
            workload=workload,
            output_dir=str(Path(output_dir).resolve()),
        )
        result = import_module(adapter["package"]).measure(request)
        return await result if inspect.isawaitable(result) else result

    def close(self):
        for worker in self.owned:
            worker.close()
        self.values.clear()
        self.owned.clear()
        self.private.cleanup()
