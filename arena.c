#include "arena.h"

typedef struct _ARENA_CONTEXT {
	PCHAR Base;
	SIZE_T Offset;
	SIZE_T Size;
} ARENA_CONTEXT, *PARENA_CONTEXT;

__declspec(thread) ARENA_CONTEXT ArenaContext = { 0 };

#define PAGE_SIZE 4096

VOID ArenaDestroy()
{
	if (ArenaContext.Base)
		VirtualFree(ArenaContext.Base, 0, MEM_RELEASE);

	RtlZeroMemory(&ArenaContext, sizeof(ArenaContext));
}

PVOID
ArenaAlloc(SIZE_T Size)
{
	SIZE_T Remaining = ArenaContext.Size - ArenaContext.Offset;
	SIZE_T AlignedSize;
	PVOID Block;

	if (Size > Remaining)
		return NULL;

	AlignedSize = (Size + 7) & ~7;
	Block = &ArenaContext.Base[ArenaContext.Offset];
	ArenaContext.Offset += AlignedSize;

	RtlZeroMemory(Block, Size);
	return Block;
}

UINT32
ArenaInit(SIZE_T Size)
{
	PVOID Base;

	if (ArenaContext.Base)
		return ERROR_ALREADY_INITIALIZED;

	Size = (Size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;

	Base = VirtualAlloc(NULL, Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!Base)
		return ERROR_NOT_ENOUGH_MEMORY;

	ArenaContext.Base = Base;
	ArenaContext.Offset = 0;
	ArenaContext.Size = Size;

	return ERROR_SUCCESS;
}