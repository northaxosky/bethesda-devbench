#pragma once

#include "CorePch.h"

#if defined(DEVBENCH_GAME_FALLOUT4)
#	include <F4SE/F4SE.h>
#	include <RE/Fallout.h>
#	include <REX/REX.h>
#elif defined(DEVBENCH_GAME_SKYRIMSE)
#	include <RE/Skyrim.h>
#	include <SKSE/SKSE.h>
#	include <SKSE/ContextHook.h>
#else
#	error "A native game adapter must be selected"
#endif

#include <Windows.h>

#ifdef ERROR
#	undef ERROR
#endif
