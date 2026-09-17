#include "GameState.h"

#include "RuntimeContext.h"

namespace dvb::game
{
	int CurrentFrame()
	{
		try
		{
			const auto& provider = GetRuntimeContext().currentFrame;
			return provider ? provider() : -1;
		}
		catch (...)
		{
			return -1;
		}
	}
}
