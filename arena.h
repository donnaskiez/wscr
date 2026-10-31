#ifndef ARENA_H
#define ARENA_H

#include <Windows.h>

UINT32
ArenaInit(SIZE_T Size);

PVOID
ArenaAlloc(SIZE_T Size);

VOID ArenaDestroy();

#endif