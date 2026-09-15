// Copyright MadFall. All Rights Reserved.

/**
 * Lua 5.4, compiled as C++ in one translation unit.
 *
 * WHY C++, AND WHY HERE
 *   Lua raises errors with longjmp when built as C. On Win64 a longjmp unwinds
 *   through every frame between the error and the protected call, and the
 *   MadFall bindings in between are C++ functions: the C++ unwinder does not
 *   expect a longjmp there and terminates the process. Built as C++, Lua raises
 *   errors with throw/catch instead (ldo.c, LUAI_THROW), which unwinds C++
 *   frames correctly and runs their destructors.
 *
 *   UBT compiles every .c file in a module as C, so the sources live outside
 *   the module (Source/ThirdParty/Lua) and are included here. The order is
 *   Lua's own onelua.c order. This file must not see Unreal headers: Lua has
 *   functions named check and verify, which Unreal defines as macros - hence
 *   no PCH and no unity build for this module.
 */

// Lua's warnings are not ours to fix, and the CI warning gate reads every
// compiler diagnostic. Silenced for the whole file, never popped: MSVC reports
// code-generation warnings (C4701, C4702) after the last line, where a pop
// would already have restored them. Nothing but Lua is in this file.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable: 4701 4702 4703)
#elif defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include "lprefix.h"

#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"

#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "lmathlib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
