"""Installed compiler candidates, each retaining its own build and effective configuration."""

from dataclasses import dataclass
from importlib import import_module
import inspect
import json
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
from uuid import uuid4
from compilerinfra.tree import provenance, relative, tree
from .source import Snapshot, adoption_lock, applied, build
from compilerinfra.installation import publish
from .worker import Worker
from compilerinfra.observations import Observations


@dataclass
class Candidate:
    worker: Worker
    snapshot: Snapshot | None
    description: dict
    parent: str | None
    changes: list


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
        self.library = (
            Path(library).parent.resolve() / Path(library).name if library else None
        )
        self.observations = Observations()
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
            if file == self.library or file.name == self.library.name + ".python":
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
            self.values[key] = Candidate(worker, snap, desc, None, [])
            self.owned[worker] = self.root / key
            self.base_key = key
        except BaseException:
            worker.close()
            raise

    @staticmethod
    def check_fingerprint(desc, model):
        actual = provenance(model)

        def python_assets(layout):
            return {
                a["path"]: a["sha256"]
                for a in layout["assets"]
                if a.get("kind") in {"frontend", "runtime"}
            }

        if python_assets(desc) != python_assets(model):
            raise ValueError(
                "workspace product modules differ from the installed manifest"
            )
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

    def source_locations(self, id):
        candidate = self.values[self.key(id)]
        layout = candidate.description
        roots = {
            k: (self.workspace / path).resolve()
            for k, path in layout["packages"].items()
        }
        if (
            candidate.snapshot
            and candidate.snapshot is not self.values[self.base_key].snapshot
        ):
            roots = candidate.snapshot.roots
        locations = {}
        for name, registration in layout["components"].items():
            unit = layout["units"][registration["unit"]]
            package = unit["package"]
            if registration["file"].startswith("@generated/"):
                locations[name] = {
                    "generated": True,
                    "file": registration["file"],
                    "line": registration["line"],
                }
                continue
            local = os.path.relpath(registration["file"], layout["packages"][package])
            locations[name] = {
                "file": str(roots[package] / local),
                "line": registration["line"],
            }
        return locations

    def derive(self, parent, configuration, patch, extensions):
        base_key = self.key(parent)
        base = self.values[base_key]
        key = uuid4().hex
        folder = self.root / key
        changes = [*base.changes, configuration]
        worker, snap = base.worker, base.snapshot
        patches = dict(extensions or {})
        if patch:
            patches["."] = patch
        try:
            if patches:
                snap = snap.derive(folder, patches)
                lib = build(snap.roots, folder / "build", folder / "install")
                worker = Worker(lib, self.profile)
                desc = worker.call("open", key, changes=changes)
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
            reverse = snap.delta(c.description["configuration"], reverse=True)
            with applied(roots, patches):
                stage = folder / "install"
                lib = build(roots, folder / "build", stage)
                rel = relative(c.description["library"])
                prefix = self.library.parents[len(rel.parts) - 1]
                if (prefix / rel) != self.library:
                    raise ValueError(
                        "installed library does not match product install entry"
                    )
                if lib.relative_to(stage) != rel:
                    raise ValueError("install did not produce expected compiler library")
                history = prefix / ".compilerinfra" / (rel.name + ".revert")
                history.mkdir(parents=True, exist_ok=True)
                pointer = history / "previous"
                older = pointer.resolve() if pointer.exists() else None
                saved = history / uuid4().hex
                saved.mkdir()
                previous = self.base_key
                try:
                    with publish(stage, prefix, folder / "backup", rel, keep_install=saved / "install"):
                        self.load_base()
                        record = {"adopted":self.values[self.base_key].description["fingerprint"],
                                  "patches":{k:v.decode() for k,v in reverse.items()}}
                        (saved / "reverse.json").write_text(json.dumps(record))
                        link = saved / "next"
                        link.symlink_to(saved.name)
                        link.replace(pointer)
                except BaseException:
                    self.base_key = previous
                    shutil.rmtree(saved)
                    raise
                if older:
                    shutil.rmtree(older)
            return {
                "base": self.base_key,
                "previous": previous,
                "library": str(self.library),
            }

    def revert(self):
        with adoption_lock(self.workspace):
            current = self.values[self.base_key].description
            model = tree(self.workspace)
            self.check_fingerprint(current, model)
            rel = relative(current["library"])
            prefix = self.library.parents[len(rel.parts)-1]
            pointer = prefix / ".compilerinfra" / (rel.name + ".revert") / "previous"
            if not pointer.is_symlink():
                raise ValueError("no previous adoption to revert")
            saved = pointer.resolve(strict=True)
            record = json.loads((saved / "reverse.json").read_text())
            if record["adopted"] != current["fingerprint"]:
                raise ValueError("current installation differs from the adopted baseline")
            roots = {k:Path(v) for k,v in model["packages"].items()}
            patches = {k:v.encode() for k,v in record["patches"].items()}
            with TemporaryDirectory(dir=self.root) as tmp, applied(roots,patches):
                # publish writes an inventory into its input stage. Keep the saved
                # installation intact if activation fails and revert is retried.
                stage = Path(tmp) / "stage"
                shutil.copytree(saved / "install", stage)
                previous = self.base_key
                with publish(stage, prefix, Path(tmp)/"backup", rel):
                    self.load_base()
                    pointer.unlink()
            shutil.rmtree(saved)
            return {"base":self.base_key,"previous":previous,"library":str(self.library)}

    def compile(
        self,
        id,
        source,
        exit,
        through=None,
        after=None,
        root_sha256=None,
        author_input=False,
    ):
        key = self.key(id)
        c = self.values[key]

        result = c.worker.call(
            "compile",
            key,
            source=source,
            exit=exit,
            through=through,
            after=after,
            text=bool(through),
            explain=self.profile == "development",
            root_sha256=root_sha256,
            author_input=author_input,
        )
        if self.profile != "development":
            return result
        artifact, report = result
        report["native_candidate"] = report["candidate"]
        report = dict(report, scope_candidate=key)
        cid = report["compilation_id"]
        self.observations[cid] = (report, c.description, artifact)
        if report["error"]:
            raise ValueError(f"{cid}: {report['error']}")
        if report.get("package_sha256"):
            from compilerinfra.measured import unpack
            _, artifact = unpack(artifact)
        return artifact, cid

    def impact(self, id, component, compilation_id=None):
        report = None
        if compilation_id is None:
            description = self.values[self.key(id or "base")].description
        else:
            report, description, _ = self.observations[compilation_id]
            if id and self.key(id) != report["scope_candidate"]:
                raise ValueError("compilation belongs to a different candidate")

        item = description["components"][component]
        at = item["at"]
        reachable = [k for k, v in description["exits"].items() if at in v["path"]]
        owner = description["units"][item["unit"]]["owner"]
        if owner.startswith("exits/"):
            reachable = [e for e in reachable if owner == "exits/" + e]
        elif owner.startswith("levels/"):
            node = owner.split("/")[-1] + "@"
            reachable = [
                e
                for e in reachable
                if any(b.startswith(node) for b in description["exits"][e]["path"])
            ]
        events = report["events"] if report else None

        def hit(event):
            action = event["action"]
            if action == "candidate":
                detail = event["details"]
                if item["kind"] == "cost_model":
                    return detail["estimated"] and component == detail.get("model")
                return (
                    item["kind"] == "local_estimator"
                    and component in detail["estimator_versions"]
                )
            if action == "no_match" or (
                action == "contribution" and not event["changed"]
            ):
                return False
            return component in (event["implementation"], event.get("family"))

        return dict(
            reachable=reachable,
            selected=[e for e in reachable if component in description["selected"][e]],
            compilation_id=compilation_id,
            observed_exit=report["exit"] if report else None,
            root_sha256=report["root_sha256"] if report else None,
            hits=None if events is None else [event for event in events if hit(event)],
        )

    async def measure(self, id, workload, output_dir, compilation_id=None):
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
            python=c.worker.interpreter,
            workload=workload,
            output_dir=str(Path(output_dir).resolve()),
        )
        if compilation_id is not None:
            report, _, artifact = self.observations[compilation_id]
            if report["scope_candidate"] != key or report["error"]:
                raise ValueError(
                    "measurement needs a successful compilation of the selected candidate"
                )
            if not report["artifact_sha256"]:
                raise ValueError("measurement requires a final emitted artifact")
            if not report.get("package_sha256"):
                raise ValueError("measure a final compilation from the author's root IR")
            from tempfile import NamedTemporaryFile
            with NamedTemporaryFile(dir=self.private.name, suffix=".cimeas") as package:
                package.write(artifact)
                package.flush()
                request["artifact_file"] = package.name
                result = import_module(adapter["package"]).measure(request)
                return await result if inspect.isawaitable(result) else result
        result = import_module(adapter["package"]).measure(request)
        return await result if inspect.isawaitable(result) else result

    def close(self):
        for worker in self.owned:
            worker.close()
        self.values.clear()
        self.owned.clear()
        self.private.cleanup()
