"""Build an organized product and run its declared first handoff."""
import argparse
import json
from pathlib import Path
import subprocess


def main(argv):
    parser = argparse.ArgumentParser(prog="compilerinfra-dev check")
    parser.add_argument("--product", type=Path, default=Path("product.json"))
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    descriptor = args.product.resolve(strict=True)
    root, build = descriptor.parent, args.build_dir.resolve()
    product = json.loads(descriptor.read_text())
    config, sample = product["build"], product["check"]
    subprocess.run(["cmake", "-S", root, "-B", build, *config.get("configure_args", [])], check=True)
    subprocess.run(["cmake", "--build", build, "--target", config["target"]], check=True)
    prefix = build / "check-install"
    install = ["cmake", "--install", build, "--prefix", prefix]
    if "component" in config:
        install += ["--component", config["component"]]
    subprocess.run(install, check=True)
    driver = prefix / config["driver"]
    subprocess.run([driver, "--layout"], check=True)
    subprocess.run([driver, "--target", sample["target"], "--through", sample["through"],
                    "--format", "asm", "--", root / sample["input"]], check=True)
