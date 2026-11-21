import {apply as mountCompiler} from "@deepseek-ai/dsh-mcp-client";
import z from "@deepseek-ai/schemastery";
import {randomUUID} from "node:crypto";

export const inject = ["tools", "skills"];
export const name = "compilerinfra";
export const Config = z.object({
  library: z.string(),
  workspace: z.string().required(),
  outputs: z.string().required(),
  profile: z.union(["runtime", "development"]).default("development"),
  namespace: z.string().default("compilerinfra"),
  launcher: z.string().default("compilerinfra-agent"),
  toolCallTimeoutMs: z.number().default(300000),
});

export async function apply(ctx, config) {
  const args = [
    "--workspace", config.workspace, "--output-dir",
    config.outputs, "--profile", config.profile
  ];
  if (config.library) args.push("--library", config.library);
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
  if (!config.library || config.profile === "runtime") return;
  const response = await call("inspect", {});
  const catalog = Array.isArray(response) ? JSON.parse(response[0].text) : response;
  for (const id of catalog.skills) {
    const body = await call("read-skill", {name: id});
    const content = Array.isArray(body) ? JSON.parse(body[0].text) : body;
    ctx.skills.register({
      name: (config.namespace + "-" + id).toLowerCase().replace(/[^a-z0-9]+/g, "-"),
      description: content.split("\n", 1)[0].replace(/^#+\s*/, ""),
      content,
      source: "runtime",
      provider: config.namespace,
    });
  }
}
