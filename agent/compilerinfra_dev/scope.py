import asyncio
import copy
from contextlib import ExitStack, asynccontextmanager
from dataclasses import dataclass
import json
from pathlib import Path
import shutil
import shlex
from tempfile import TemporaryDirectory
from uuid import uuid4

from .plugins import load
from .process import execute
from .source import Sources, merge, relative


@dataclass(frozen=True)
class Candidate:
    name: str
    parent: str | None
    tree: str | None
    install: Path | None
    driver: Path
    preset: dict
    preset_file: Path | None
    source_root: Path


class BoundCompiler:
    def __init__(self, scope, value):
        self.scope, self.value = scope, value

    @property
    def product(self):
        return self.scope.configs[self.value.name]

    def executable(self, binary, args=(), wrapper_name="product-driver"):
        """Bind preset and product arguments for callers which launch the tool directly."""
        options = list(args)
        if self.value.preset_file:
            options.extend(("--preset", str(self.value.preset_file)))
        if not options:
            return binary
        folder = (
            self.scope.private
            if self.value.name == "base"
            else self.scope.candidates / self.value.name
        )
        path = folder / wrapper_name
        if not path.exists():
            path.write_text(
                "#!/bin/sh\nexec " + shlex.join([str(binary), *options]) + ' "$@"\n'
            )
            path.chmod(0o700)
        return path

    async def run(self, *args, data=None):
        if self.value.preset_file:
            args = ("--preset", str(self.value.preset_file), *args)
        cwd = self.value.source_root if self.value.tree else self.scope.workspace
        return await execute(
            [self.value.driver, *self.scope.args, *args], cwd, data=data
        )

    async def query(self, *args):
        out, _ = await self.run(*args)
        return json.loads(out)

    def source(self, path):
        source = (self.scope.workspace / path).resolve(strict=True)
        if not (
            source.is_relative_to(self.scope.workspace)
            or source.is_relative_to(self.scope.outputs)
        ):
            raise ValueError("source lies outside workspace and outputs")
        return source

    def output(self, path):
        dest = (self.scope.outputs / relative(path)).resolve()
        if not dest.is_relative_to(self.scope.outputs) or dest.is_relative_to(
            self.scope.private
        ):
            raise ValueError("output leaves product output directory")
        dest.parent.mkdir(parents=True, exist_ok=True)
        return dest


class Scope:
    def __init__(
        self, driver, workspace, outputs, args=(), product=None, bundle="development"
    ):
        self.workspace = Path(workspace).resolve(strict=True)
        self.outputs = Path(outputs).resolve()
        self.outputs.mkdir(parents=True, exist_ok=True)
        self.temp = TemporaryDirectory(prefix=".compiler-", dir=self.outputs)
        self.private = Path(self.temp.name)
        self.candidates = self.private / "candidates"
        self.candidates.mkdir()
        self.args = tuple(args)
        self.bundle = bundle
        self.product = product or {}
        self.sources = Sources(
            self.workspace, self.private, self.product.get("build", {})
        )
        self.driver = Path(driver).resolve(strict=True)
        self.values = {}
        self.parents = {}
        self.adoptions = {}
        self.plugins = {}
        self.configs = {}
        self.leases = {}
        self.lock = asyncio.Lock()
        self.idle = asyncio.Event()
        self.idle.set()
        self.closing = False

    async def start(self):
        source = self.private / "base-source"
        if self.sources.config:
            tree = await self.sources.start()
            await self.sources.export(tree, source)
        else:
            tree = None
            source.mkdir()
            from .plugins import snapshot_plugins

            snapshot_plugins(self.workspace, source, self.product.get("plugins", []))
        base_driver = self.driver
        install = None
        if base_install := self.product.get("base_install"):
            prefix = Path(base_install).resolve(strict=True)
            rel = self.driver.relative_to(prefix)
            if self.private.is_relative_to(prefix):
                raise ValueError("scope output must be outside base_install")
            install = self.private / "base-install"
            shutil.copytree(prefix, install, symlinks=True)
            base_driver = install / rel
        elif tree and self.sources.config.get("target"):
            install = self.private / "base-install"
            base_driver = await self.sources.compile(
                tree, install, self.product["build"]
            )
        else:
            # Preserve $ORIGIN and $ORIGIN/../lib lookup for packaged tools.
            # External SDKs remain dependencies of the installation.
            mirror = self.private / "base-tool"
            folder = mirror / self.driver.parent.name
            folder.mkdir(parents=True)
            for path in self.driver.parent.parent.iterdir():
                if path != self.driver.parent:
                    (mirror / path.name).symlink_to(path)
            for path in self.driver.parent.iterdir():
                if path != self.driver:
                    (folder / path.name).symlink_to(path)
            base_driver = folder / self.driver.name
            shutil.copy2(self.driver, base_driver)
        base = Candidate("base", None, tree, install, base_driver, {}, None, source)
        self.activate(base, source)
        self.values["base"] = base

    def product_at(self, src):
        if self.sources.config and "_descriptor" in self.product:
            path = src / relative(self.product["_descriptor"])
            return json.loads(path.read_text())
        return self.product

    def activate(self, candidate, src):
        builtin = Path(__file__).parent / "bundles"
        packages = json.loads((builtin / "presets.json").read_text())[self.bundle]
        paths = [builtin / package / "plugin.json" for package in packages]
        product = self.product_at(src)
        paths.extend(src / relative(p) for p in product.get("plugins", []))
        self.configs[candidate.name] = product
        self.plugins[candidate.name] = load(paths, BoundCompiler(self, candidate))

    @asynccontextmanager
    async def use(self, name):
        if self.closing:
            raise ValueError("compiler scope is closing")
        value = self.values[name]
        self.idle.clear()
        self.leases[name] = self.leases.get(name, 0) + 1
        try:
            yield BoundCompiler(self, value)
        finally:
            self.leases[name] -= 1
            if not any(self.leases.values()):
                self.idle.set()

    async def derive(self, parent, preset, source_patch):
        async with self.lock:
            if self.closing:
                raise ValueError("compiler scope is closing")
            base = self.values[parent]
            if not preset and not source_patch:
                raise ValueError("derive requires a preset or source patch")
            name = uuid4().hex
            folder = self.candidates / name
            folder.mkdir()
            try:
                install, tree, driver = base.install, base.tree, base.driver
                src = base.source_root
                if source_patch:
                    if not self.sources.config.get("target"):
                        raise ValueError("product has not configured a source build")
                    initial = await self.sources.start()
                    tree = await self.sources.change(tree or initial, source_patch)
                    install = folder / "install"
                    src = folder / "source"
                    await self.sources.export(tree, src)
                    driver = await self.sources.compile(
                        tree, install, self.product_at(src)["build"]
                    )
                merged = merge(copy.deepcopy(base.preset), preset)
                path = None
                if merged:
                    path = folder / "preset.json"
                    path.write_text(json.dumps(merged))
                candidate = Candidate(
                    name, parent, tree, install, driver, merged, path, src
                )
                bound = BoundCompiler(self, candidate)
                await bound.query("--components")
                entry = bound.executable(driver, self.args)
                self.activate(candidate, src)
                self.values[name] = candidate
                self.parents[name] = parent
                return {
                    "compiler": name,
                    "parent": parent,
                    "driver": str(entry),
                    "cwd": str(src if tree else self.workspace),
                    "preset_file": str(path) if path else None,
                    "installation": str(install) if install else None,
                    "source_changed": bool(source_patch),
                }
            except BaseException:
                installed = self.plugins.pop(name, None)
                self.configs.pop(name, None)
                if installed:
                    installed.close()
                shutil.rmtree(folder)
                raise

    async def adopt(self, name):
        async with self.lock:
            if name == "base":
                raise ValueError("select a derived compiler")
            value = self.values[name]
            initial = await self.sources.start()
            ancestor = name
            while ancestor not in self.adoptions and ancestor != "base":
                ancestor = self.parents[ancestor]
            tree, changed = await self.sources.adopt(
                value.tree or initial,
                value.preset,
                self.adoptions.get(ancestor, initial),
                self.configs[name].get("build", {}),
            )
            self.adoptions[name] = tree
            return {
                "compiler": name,
                "workspace": str(self.workspace),
                "applied": changed,
            }

    async def discard(self, name):
        async with self.lock:
            if name == "base" or self.leases.get(name):
                raise ValueError("cannot discard base or an active invocation")
            del self.values[name]
            self.plugins.pop(name).close()
            del self.configs[name]
            self.leases.pop(name, None)
            # Config-only descendants can retain a discarded ancestor's installation.
            live = {v.install.parent for v in self.values.values() if v.install}
            live.update(v.source_root.parent for v in self.values.values())
            for folder in self.candidates.iterdir():
                if folder.name not in self.values and folder not in live:
                    shutil.rmtree(folder)
            return {"discarded": name}

    async def close(self):
        self.closing = True
        await self.idle.wait()
        async with self.lock:
            try:
                with ExitStack() as cleanup:
                    for plugin in self.plugins.values():
                        cleanup.callback(plugin.close)
            finally:
                self.plugins.clear()
                self.values.clear()
                self.temp.cleanup()
