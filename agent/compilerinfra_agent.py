"""Stdio MCP, a persistent JSON-lines CLI, and one-shot SDK actions."""

import argparse
import asyncio
import json
from pathlib import Path
import sys
from compilerinfra_dev.actions import Catalog
from compilerinfra_dev.scope import Scope


def options():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--workspace", type=Path, default=Path.cwd())
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument(
        "--profile", choices=["runtime", "development"], default="development"
    )
    parser.add_argument("action", nargs="?")
    parser.add_argument("arguments", nargs="?", default="{}")
    return parser.parse_args()


async def host(mcp):
    opts = options()
    scope = Scope(
        opts.workspace,
        opts.output_dir or opts.workspace / ".work" / "development",
        opts.library,
        opts.profile,
    )
    catalog = Catalog(scope)
    lock = asyncio.Lock()

    async def invoke(name, args):
        async with lock:
            return await catalog.invoke(name, args)

    try:
        if opts.action:
            value = await invoke(opts.action, json.loads(opts.arguments))
            print(
                value
                if isinstance(value, str)
                else json.dumps(value, ensure_ascii=False)
            )
        elif mcp:
            from mcp.server import Server
            from mcp.server.stdio import stdio_server
            from mcp.types import TextContent, Tool

            server = Server("compilerinfra")

            @server.list_tools()
            async def tools():
                return [
                    Tool(
                        name=id,
                        description=s["description"] or id,
                        inputSchema=s["parameters"],
                    )
                    for id, s in catalog.schemas().items()
                ]

            @server.call_tool()
            async def call(name, arguments):
                value = await invoke(name, arguments)
                return [
                    TextContent(type="text", text=json.dumps(value, ensure_ascii=False))
                ]

            async with stdio_server() as (reader, writer):
                await server.run(reader, writer, server.create_initialization_options())
        else:
            for line in sys.stdin:
                try:
                    req = json.loads(line)
                    result = {
                        "result": await invoke(req["action"], req.get("arguments", {}))
                    }
                except Exception as exc:
                    result = {"error": str(exc)}
                print(json.dumps(result, ensure_ascii=False), flush=True)
    finally:
        scope.close()


def main():
    asyncio.run(host(True))


def cli():
    asyncio.run(host(False))


if __name__ == "__main__":
    cli()
