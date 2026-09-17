export interface SetupArguments {
  game: string;
  install?: string;
  runtimeFile?: string;
  config?: string;
}

export function printSetupSnippet(
  command: string,
  scriptArgs: string[],
  args: SetupArguments,
): void {
  const cliArgs = [
    ...scriptArgs,
    "--game",
    args.game,
    ...(args.install ? ["--install", args.install] : []),
    ...(args.runtimeFile ? ["--runtime-file", args.runtimeFile] : []),
    ...(args.config ? ["--config", args.config] : []),
  ];
  const snippet = {
    mcpServers: {
      [`devbench-${args.game}`]: {
        command,
        args: cliArgs,
      },
    },
  };
  console.log(JSON.stringify(snippet, null, 2));
}
