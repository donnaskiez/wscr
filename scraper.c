#include "scraper.h"

#pragma comment(lib, "Shlwapi.lib")
#include <shlwapi.h>

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

typedef struct _SESSION_CONTEXT {
	HINTERNET SessionHandle;
	HINTERNET ConnectionHandle;

	struct {
		URL_COMPONENTSA Components;
		HINTERNET RequestHandle;
	} BaseUrl;

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

typedef struct _PAGE {
	URL_COMPONENTSA Components;
	PCHAR AbsoluteUrl;
	DWORD AbsoluteUrlLength;
} PAGE, *PPAGE;

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
			printf("AbsoluteURL: %s\n", ChildPage->AbsoluteUrl);
			printf("AbsoluteURL Length: %d\n", ChildPage->AbsoluteUrlLength);
		}

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

/* https://quotes.toscrape.com/ */
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

	Ret = SessionInit(&Ctx, Url);
	if (Ret) {
		printf("Failed to initialise session: %d", Ret);
		return Ret;
	}

	Ret = UrlDownload(&Ctx, &Ctx.BaseUrl.Components,
					  &Ctx.BaseUrl.RequestHandle);
	if (Ret) {
		printf("Failed to download URL: %d", Ret);
		return Ret;
	}

	Ret = PageChildrenParse(&Ctx, Ctx.BaseUrl.RequestHandle,
							&Ctx.BaseUrl.Components);
	if (Ret) {
		printf("Failed to parse page childen: %d", Ret);
		return Ret;
	}

	ArenaDestroy();

	return Ret;
}