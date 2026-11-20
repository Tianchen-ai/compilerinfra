"""Configuration shared by external measurement adapters."""

from pathlib import Path
from .source import relative


def settings(compiler, name):
    config = compiler.product.get("bench", {}).get(name)
    if not config:
        raise ValueError(f"product has no Bench binding for {name}")
    return config


def output(config, name):
    root = Path(config["output_root"]).resolve()
    dest = (root / relative(name)).resolve()
    if not dest.is_relative_to(root):
        raise ValueError("output outside Bench root")
    dest.parent.mkdir(parents=True, exist_ok=True)
    return dest
