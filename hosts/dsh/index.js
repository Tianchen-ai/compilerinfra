import {apply as mountCompiler} from "@deepseek-ai/dsh-mcp-client";
import z from "@deepseek-ai/schemastery";
import {randomUUID} from "node:crypto";

export const inject = ["tools", "skills"];
export const name = "compilerinfra";
export const Config = z.object({
  driver: z.string().required(),
  workspace: z.string().required(),
  outputs: z.string().required(),
  product: z.string(),
  bundle: z.union(["runtime", "development", "measurement"]).default("development"),
  namespace: z.string().default("compilerinfra"),
  launcher: z.string().default("compilerinfra-agent"),
  driverArgs: z.array(String).default([]),
  toolCallTimeoutMs: z.number().default(300000),
});

export async function apply(ctx, config) {
  const args = [
    "--driver", config.driver, "--workspace", config.workspace, "--output-dir",
    config.outputs, "--bundle", config.bundle
  ];
  if (config.product) args.push("--product", config.product);
  for (const arg of config.driverArgs) args.push("--driver-arg=" + arg);
  // Use DSH's scoped, disposable native tool bridge for this compiler. Skills
  // and action schemas come from this exact same candidate's plugin catalog.
  await mountCompiler(ctx, {
    transport: "stdio",
    serverName: config.namespace,
    command: config.launcher,
    args,
    cwd: config.workspace,
    env: {},
    toolCallTimeoutMs: config.toolCallTimeoutMs,
    failOnStartupError: true,
  });
  async function call(name, args) {
    const result = await ctx.tools.execute({
      callId: randomUUID(),
      name: `mcp__${config.namespace}__${name}`,
      arguments: args,
      signal: AbortSignal.timeout(config.toolCallTimeoutMs),
    });
    if (result.isError) throw new Error(result.error.message);
    return result.value.structuredContent ?? result.value.content;
  }
  const catalog = await call("inspect_compiler", {});
  for (const id of catalog.skills) {
    const body = await call("read_skill", {skill: id});
    const content = typeof body.result === "string" ? body.result : body[0].text;
    ctx.skills.register({
      name: (config.namespace + "-" + id).toLowerCase().replace(/[^a-z0-9]+/g, "-"),
      description: content.split("\n", 1)[0].replace(/^#+\s*/, ""),
      content,
      source: "runtime",
      provider: config.namespace,
    });
  }
}
