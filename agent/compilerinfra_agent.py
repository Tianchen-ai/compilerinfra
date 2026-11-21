"""Host bindings for a scoped product compiler."""

import argparse
import asyncio
from contextlib import asynccontextmanager
import json
from pathlib import Path
import sys

from mcp.server.fastmcp import FastMCP

from compilerinfra_dev.actions import catalog
from compilerinfra_dev.scope import Scope


def server(driver, workspace, outputs, args=(), product=None, bundle="development"):
    scope = Scope(driver, workspace, outputs, args, product, bundle)

    @asynccontextmanager
    async def lifetime(_):
        try:
            await scope.start()
            for name, (fn, _) in catalog(scope).actions.items():
                app.add_tool(fn, name=name)
            yield scope
        finally:
            await scope.close()

    app = FastMCP("CompilerInfra", lifespan=lifetime)
    return app


def options():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--driver-arg", action="append", default=[])
    parser.add_argument(
        "--product", type=Path, help="Product development configuration (JSON)"
    )
    parser.add_argument(
        "--bundle",
        choices=["runtime", "development", "measurement"],
        default="development",
    )
    c = parser.parse_args()
    product = json.loads(c.product.read_text()) if c.product else {}
    if c.product and c.product.resolve().is_relative_to(c.workspace.resolve()):
        product["_descriptor"] = str(
            c.product.resolve().relative_to(c.workspace.resolve())
        )
    return (c.driver, c.workspace, c.output_dir, c.driver_arg, product, c.bundle)


def main():
    server(*options()).run(transport="stdio")


def cli():
    """Persistent JSON-lines CLI. Each request names one of the MCP actions."""

    if sys.argv[1:2] == ["check"]:
        from compilerinfra_dev.check import main as check
        return check(sys.argv[2:])

    async def run():
        scope = Scope(*options())
        try:
            await scope.start()
            actions = catalog(scope)
            for line in sys.stdin:
                try:
                    request = json.loads(line)
                    result = (
                        actions.describe()
                        if request["action"] == "actions"
                        else await actions.invoke(
                            request["action"], request.get("arguments", {})
                        )
                    )
                    response = {"result": result}
                except Exception as err:
                    response = {"error": str(err)}
                print(json.dumps(response), flush=True)
        finally:
            await scope.close()

    asyncio.run(run())


if __name__ == "__main__":
    main()
