#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wininet.h>
#include <shlwapi.h>

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Shlwapi.lib")

typedef struct _ARENA_CONTEXT {
	PCHAR Base;
	SIZE_T Offset;
	SIZE_T Size;
} ARENA_CONTEXT, *PARENA_CONTEXT;

__declspec(thread) ARENA_CONTEXT ArenaContext = { 0 };

#define PAGE_SIZE 4096
#define STATIC static

STATIC
VOID ArenaDestroy()
{
	if (ArenaContext.Base)
		VirtualFree(ArenaContext.Base, 0, MEM_RELEASE);

	RtlZeroMemory(&ArenaContext, sizeof(ArenaContext));
}

STATIC
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

STATIC
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

typedef struct _PAGE {
	struct _PAGE *Next;
	URL_COMPONENTSA Components;
	PCHAR AbsoluteUrl;
	DWORD AbsoluteUrlLength;
	UINT64 AbsoluteUrlHash;
} PAGE, *PPAGE;

//typedef struct _PAGE_QUEUE {
//
//}PAGE_QUEUE, *PPAGE_QUEUE;

#define HASH_SET_DEFAULT_BUCKET_COUNT 64

typedef struct _HASH_SET {
	PPAGE *BucketArray;
	UINT32 BucketCount;
} HASH_SET, *PHASH_SET;

#define FNV_HASH_OFFSET_BASIS 0xcbf29ce484222325
#define FNV_HASH_PRIME 0x00000100000001b3

STATIC
UINT64
HashSetHash(_In_ PPAGE Page)
{
	UINT64 Hash = FNV_HASH_OFFSET_BASIS;
	UINT32 Idx;

	for (Idx = 0; Idx < Page->AbsoluteUrlLength; Idx++) {
		Hash *= FNV_HASH_PRIME;
		Hash ^= (UCHAR)Page->AbsoluteUrl[Idx];
	}

	return Hash;
}

STATIC
BOOL HashSetPageEquivalent(_In_ PPAGE Page1, _In_ PPAGE Page2)
{
	if (Page1->AbsoluteUrlHash != Page2->AbsoluteUrlHash)
		return FALSE;
	if (Page1->AbsoluteUrlLength != Page2->AbsoluteUrlLength)
		return FALSE;
	if (RtlCompareMemory(Page1->AbsoluteUrl, Page2->AbsoluteUrl,
						 Page1->AbsoluteUrlLength) != Page1->AbsoluteUrlLength)
		return FALSE;
	return TRUE;
}

#define HASH_SET_PAGE_ADDED 0
#define HASH_SET_PAGE_EXIST 1

STATIC
UINT32
HashSetAddOrContains(_In_ PHASH_SET Set, _In_ PPAGE Page)
{
	UINT32 BucketIdx;
	PPAGE Cur;

	Page->AbsoluteUrlHash = HashSetHash(Page);
	BucketIdx = (UINT32)(Page->AbsoluteUrlHash & (Set->BucketCount - 1));

	for (Cur = Set->BucketArray[BucketIdx]; Cur; Cur = Cur->Next) {
		if (HashSetPageEquivalent(Cur, Page))
			return HASH_SET_PAGE_EXIST;
	}

	Page->Next = Set->BucketArray[BucketIdx];
	Set->BucketArray[BucketIdx] = Page;
	return HASH_SET_PAGE_ADDED;
}

STATIC
UINT32
HashSetInit(_Out_ PHASH_SET Set)
{
	Set->BucketArray =
			ArenaAlloc(HASH_SET_DEFAULT_BUCKET_COUNT * sizeof(PPAGE));
	if (!Set->BucketArray)
		return ERROR_NOT_ENOUGH_MEMORY;

	Set->BucketCount = HASH_SET_DEFAULT_BUCKET_COUNT;
	return ERROR_SUCCESS;
}

typedef struct _SESSION_CONTEXT {
	HINTERNET SessionHandle;
	HINTERNET ConnectionHandle;

	struct {
		URL_COMPONENTSA Components;
		HINTERNET RequestHandle;
	} BaseUrl;

	HASH_SET Set;

} SESSION_CONTEXT, *PSESSION_CONTEXT;

STATIC
UINT32
UrlDownload(_In_ PSESSION_CONTEXT Ctx, _In_ LPURL_COMPONENTSA Url,
			_Out_ LPHINTERNET RequestHandle)
{
	DWORD BufLength = sizeof(DWORD);
	DWORD ContentLength = 0;
	DWORD StatusCode = 0;
	DWORD Flags = 0;
	BOOL Ret;

	if (Url->nScheme == INTERNET_SCHEME_HTTPS)
		Flags |= INTERNET_FLAG_SECURE;

	*RequestHandle = HttpOpenRequestA(Ctx->ConnectionHandle, "GET",
									  Url->lpszUrlPath, NULL, NULL, NULL, Flags,
									  NULL);
	if (!*RequestHandle) {
		printf("Failed to open HttpRequest handle: %d", GetLastError());
		return ERROR_BAD_ARGUMENTS;
	}

	HttpSendRequestA(*RequestHandle, NULL, 0, NULL, 0);

	Ret = HttpQueryInfoA(*RequestHandle,
						 HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
						 &StatusCode, &BufLength, NULL);
	if (!Ret) {
		printf("Failied to query status code: %d", GetLastError());
		return ERROR_ACCESS_DENIED;
	}

	if (StatusCode != HTTP_STATUS_OK) {
		printf("HTTPS Query Failed. Status: %d", StatusCode);
		return ERROR_ACCESS_DENIED;
	}

	return ERROR_SUCCESS;
}

STATIC
UINT32
UrlParse(_In_ LPCSTR Url, _Out_ LPURL_COMPONENTSA Components)
{
	BOOL Ret;

	Components->dwStructSize = sizeof(*Components);
	Components->dwHostNameLength = 1;
	Components->lpszHostName = NULL;
	Components->dwUrlPathLength = 1;
	Components->lpszUrlPath = NULL;
	Components->dwSchemeLength = 1;
	Components->lpszScheme = NULL;
	Components->dwExtraInfoLength = 1;
	Components->lpszExtraInfo = NULL;

	Ret = InternetCrackUrlA(Url, (DWORD)strlen(Url), 0, Components);
	if (!Ret) {
		printf("Failed to retrieve required buf sizes: %d\n", GetLastError());
		return ERROR_BAD_ARGUMENTS;
	}

	Components->lpszHostName = ArenaAlloc(Components->dwHostNameLength + 1);
	if (!Components->lpszHostName) {
		printf("Failed to allocate HostName buffer: %d\n", GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwHostNameLength++;
	Components->lpszUrlPath = ArenaAlloc(Components->dwUrlPathLength + 1);
	if (!Components->lpszUrlPath) {
		printf("Failed to allocate URLPath buffer: %d\n", GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwUrlPathLength++;
	Components->lpszScheme = ArenaAlloc(Components->dwSchemeLength + 1);
	if (!Components->lpszScheme) {
		printf("Failed to allocate URL Scheme buffer: %d\n", GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwSchemeLength++;
	Components->lpszExtraInfo = ArenaAlloc(Components->dwExtraInfoLength + 1);
	if (!Components->lpszExtraInfo) {
		printf("Failed to allocate URL Extra Info buffer: %d\n",
			   GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwExtraInfoLength++;

	Ret = InternetCrackUrlA(Url, (DWORD)strlen(Url), 0, Components);
	if (!Ret) {
		printf("Failed to parse URL: %d\n", GetLastError());
		return ERROR_BAD_ARGUMENTS;
	}

	return ERROR_SUCCESS;
}

STATIC
UINT32
SessionInit(_Out_ PSESSION_CONTEXT Ctx, _In_ LPCSTR Url)
{
	UINT32 Ret;

	Ret = UrlParse(Url, &Ctx->BaseUrl.Components);
	if (Ret != ERROR_SUCCESS) {
		printf("Failed to parse URL: %d\n", Ret);
		return Ret;
	}

	Ctx->SessionHandle = InternetOpenA("WinInet", INTERNET_OPEN_TYPE_PRECONFIG,
									   NULL, NULL, 0);
	if (!Ctx->SessionHandle) {
		printf("Failed to open SessionHandle: %d\n", GetLastError());
		return GetLastError();
	}

	Ctx->ConnectionHandle = InternetConnectA(
			Ctx->SessionHandle, Ctx->BaseUrl.Components.lpszHostName,
			Ctx->BaseUrl.Components.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0,
			NULL);
	if (!Ctx->ConnectionHandle) {
		printf("Failed to open ConnectionHandle: %d\n", GetLastError());
		return GetLastError();
	}

	return ERROR_SUCCESS;
}

STATIC
PVOID
UrlBuildAbsoluteFromRelative(_In_ PSESSION_CONTEXT Ctx, _In_ LPCSTR Relative,
							 _In_ DWORD Length)
{
	CHAR BaseUrlString[INTERNET_MAX_URL_LENGTH] = { 0 };
	DWORD BaseUrlLength = INTERNET_MAX_URL_LENGTH;
	DWORD AbsoluteLength = 0;
	PCHAR Absolute;

	if (!InternetCreateUrlA(&Ctx->BaseUrl.Components, 0, BaseUrlString,
							&BaseUrlLength)) {
		printf("Failed to rebuild base URL string: %d\n", GetLastError());
		return NULL;
	}

	InternetCombineUrlA(BaseUrlString, Relative, NULL, &AbsoluteLength,
						ICU_BROWSER_MODE);

	Absolute = ArenaAlloc(AbsoluteLength);
	if (!Absolute)
		return NULL;

	if (!InternetCombineUrlA(BaseUrlString, Relative, Absolute, &AbsoluteLength,
							 ICU_BROWSER_MODE)) {
		printf("InternetCombineUrlA failed: %d\n", GetLastError());
		return NULL;
	}

	return Absolute;
}

/*
 * Rather then reformatting the string, find the fragment '#' and
 * replace it with the NUL terminator.
 */
STATIC
VOID UrlStripFragment(_Inout_ PCHAR Url)
{
	PCHAR Fragment = StrChrA(Url, '#');

	if (Fragment)
		*Fragment = '\0';
}

STATIC
PPAGE
PageChildrenParseEntry(_In_ PSESSION_CONTEXT Ctx, _In_ PVOID Base,
					   _In_ DWORD Length)
{
	UINT32 Ret;
	PPAGE Page;
	PCHAR Url;

	Page = ArenaAlloc(sizeof(*Page));
	if (!Page)
		return NULL;

	Url = ArenaAlloc(Length + 1);
	if (!Url)
		return NULL;

	RtlCopyMemory(Url, Base, Length);

	/* If its a relative, rebuild absolute */
	if (!UrlIsA(Url, URLIS_URL)) {
		Url = UrlBuildAbsoluteFromRelative(Ctx, Url, Length);
		if (!Url) {
			printf("Failed to build absolute URL: %lu\n", GetLastError());
			return NULL;
		}
	}

	UrlStripFragment(Url);

	/* TODO: lower case scheme + hostname */

	Ret = UrlParse(Url, &Page->Components);
	if (Ret != ERROR_SUCCESS) {
		printf("Failed to parse URL: %u\n", Ret);
		return NULL;
	}

	/* Remove URLs that don't match our base hostname */
	if (StrCmpA(Page->Components.lpszHostName,
				Ctx->BaseUrl.Components.lpszHostName))
		return NULL;

	Page->AbsoluteUrl = Url;
	Page->AbsoluteUrlLength = strlen(Url);
	return Page;
}

STATIC
VOID PageChildrenEnumerate(_In_ PSESSION_CONTEXT Ctx, _In_ PVOID Page)
{
	PCHAR PageIndex = (PCHAR)Page;
	PCHAR String = "a href=\"";
	PPAGE ChildPage;
	UINT8 Ret;

	while (TRUE) {
		PCHAR Href = StrStrA(PageIndex, String);
		UINT32 UrlLen;
		PCHAR UrlEnd;

		if (!Href)
			break;

		/* a href=" */
		Href += strlen(String);

		/* Find end of URL */
		UrlEnd = StrChrA(Href, '"');
		if (!UrlEnd)
			break;

		/* Subtract offset for length of URL */
		UrlLen = UrlEnd - Href;

		ChildPage = PageChildrenParseEntry(Ctx, Href, UrlLen);
		if (ChildPage) {
			Ret = HashSetAddOrContains(&Ctx->Set, ChildPage);
			if (Ret == HASH_SET_PAGE_EXIST)
				goto increment;

			printf("Adding new page to queue!\n");
		}

increment:
		PageIndex += ((Href - PageIndex) + UrlLen);
	}
}

#define PAGE_READ_CHUNK (64 * 1024)

STATIC
UINT32 PageChildrenParse(_In_ PSESSION_CONTEXT Ctx, _In_ HINTERNET Request,
						 _In_ LPURL_COMPONENTSA Url)
{
	SIZE_T Capacity = PAGE_READ_CHUNK;
	SIZE_T Length = 0;
	DWORD BytesRead;
	PCHAR Page;

	Page = ArenaAlloc(Capacity);
	if (!Page)
		return ERROR_NOT_ENOUGH_MEMORY;

	/* The last byte is kept free for the NUL terminator */
	do {
		if (Capacity - Length == 1) {
			if (ArenaAlloc(PAGE_READ_CHUNK) != Page + Capacity)
				return ERROR_NOT_ENOUGH_MEMORY;

			Capacity += PAGE_READ_CHUNK;
		}

		if (!InternetReadFile(Request, Page + Length,
							  (DWORD)(Capacity - Length - 1), &BytesRead)) {
			printf("Failed to read page. %d", GetLastError());
			return ERROR_ACCESS_DENIED;
		}

		Length += BytesRead;
	} while (BytesRead);

	Page[Length] = '\0';
	PageChildrenEnumerate(Ctx, Page);

	return ERROR_SUCCESS;
}

/*
 * https://quotes.toscrape.com/
 * https://crawler-test.com/
 */
STATIC
UINT32
Scrape(_In_ LPCSTR Url)
{
	SESSION_CONTEXT Ctx = { 0 };
	UINT32 Ret;

	Ret = ArenaInit(16 * 1024 * 1024);
	if (Ret) {
		printf("Failed to initialise base allocator");
		return Ret;
	}

	Ret = HashSetInit(&Ctx.Set);
	if (Ret) {
		printf("Failed to initialise HashSet");
		goto end;
	}

	Ret = SessionInit(&Ctx, Url);
	if (Ret) {
		printf("Failed to initialise session: %d", Ret);
		goto end;
	}

	Ret = UrlDownload(&Ctx, &Ctx.BaseUrl.Components,
					  &Ctx.BaseUrl.RequestHandle);
	if (Ret) {
		printf("Failed to download URL: %d", Ret);
		goto end;
	}

	Ret = PageChildrenParse(&Ctx, Ctx.BaseUrl.RequestHandle,
							&Ctx.BaseUrl.Components);
	if (Ret) {
		printf("Failed to parse page childen: %d", Ret);
		goto end;
	}

end:
	ArenaDestroy();
	return Ret;
}

INT WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
				   _In_ LPSTR lpCmdLine, _In_ INT nShowCmd)
{
	printf("arg count: %d\n", __argc);
	if (__argc > 1)
		printf("%s\n", __argv[1]);

	Scrape(__argv[1]);

	return 0;
}
