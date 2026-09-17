#include "PingTool.h"

namespace dvb::tools
{
	ToolDescriptor BuildPingDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "ping";
		descriptor.description = "devbench C-ABI self-test; echoes its args.";
		descriptor.inputSchema = DefaultInputSchema();
		descriptor.readOnly = true;
		return descriptor;
	}
}
