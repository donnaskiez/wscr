#include "scraper.h"

#pragma comment(lib, "Shlwapi.lib")
#include <shlwapi.h>

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

	Ret = InternetCrackUrlA(Url, (DWORD)strlen(Url), 0, Components);
	if (!Ret) {
		printf("Failed to retrieve required buf sizes: %d\n", GetLastError());
		return ERROR_BAD_ARGUMENTS;
	}

	Components->lpszHostName = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
										 Components->dwHostNameLength + 1);
	if (!Components->lpszHostName) {
		printf("Failed to allocate HostName buffer: %d\n", GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwHostNameLength++;
	Components->lpszUrlPath = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
										Components->dwUrlPathLength + 1);
	if (!Components->lpszUrlPath) {
		printf("Failed to allocate URLPath buffer: %d\n", GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwUrlPathLength++;
	Components->lpszScheme = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
									   Components->dwSchemeLength + 1);
	if (!Components->lpszScheme) {
		printf("Failed to allocate URL Scheme buffer: %d\n", GetLastError());
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Components->dwSchemeLength++;

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

	Ret = UrlParse(Url, &Ctx->BaseUrl);
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

	Absolute = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, AbsoluteLength);
	if (!Absolute)
		return NULL;

	if (!InternetCombineUrlA(BaseUrlString, Relative, Absolute, &AbsoluteLength,
							 ICU_BROWSER_MODE)) {
		printf("InternetCombineUrlA failed: %d\n", GetLastError());
		return NULL;
	}

	printf("AbsoluteURL: %s\n", Absolute);
	return Absolute;
}

typedef struct _PAGE {
	URL_COMPONENTSA Components;
} PAGE, *PPAGE;

STATIC
PPAGE
PageChildrenParseEntry(_In_ PSESSION_CONTEXT Ctx, _In_ PVOID Base,
					   _In_ DWORD Length)
{
	PVOID AbsoluteUrl;
	PCHAR String;
	UINT32 Ret;
	PPAGE Page;

	Page = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Page));
	if (!Page)
		return NULL;

	String = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Length + 1);
	if (!String)
		return NULL;

	RtlCopyMemory(String, Base, Length);

	/* Check if it is already an absolute URL */
	if (UrlIsA(String, URLIS_URL)) {
		Ret = UrlParse(String, &Page->Components);
		if (Ret != ERROR_SUCCESS) {
			printf("Failed to parse URL: %d\n", Ret);
			return NULL;
		}
	} else {
		AbsoluteUrl = UrlBuildAbsoluteFromRelative(Ctx, String, Length);
		if (!AbsoluteUrl) {
			printf("Failed to build absolute URL: %d", GetLastError());
			return NULL;
		}

		Ret = UrlParse(AbsoluteUrl, &Page->Components);
		if (Ret != ERROR_SUCCESS) {
			printf("Failed to parse combined URL components: %d\n", Ret);
			return NULL;
		}
	}

	return Page;
}

STATIC
VOID PageChildrenEnumerate(_In_ PSESSION_CONTEXT Ctx, _In_ PVOID Page)
{
	PCHAR PageIndex = (PCHAR)Page;
	PCHAR String = "a href=\"";

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

		PageChildrenParseEntry(Ctx, Href, UrlLen);

		PageIndex += ((Href - PageIndex) + UrlLen);
	}
}

STATIC
UINT32 PageChildrenParse(_In_ PSESSION_CONTEXT Ctx, _In_ HINTERNET Request,
						 _In_ LPURL_COMPONENTSA Url)
{
	DWORD BufLength = sizeof(DWORD);
	DWORD ContentLength = 0;
	DWORD BytesRead;
	PVOID Page;
	UINT32 Ret;

	Ret = HttpQueryInfoA(Request,
						 HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
						 &ContentLength, &BufLength, NULL);
	if (!Ret) {
		printf("Failed to send request: %d", GetLastError());
		return ERROR_ACCESS_DENIED;
	}

	Page = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ContentLength);
	if (!Page)
		return ERROR_NOT_ENOUGH_MEMORY;

	if (!InternetReadFile(Request, Page, ContentLength, &BytesRead)) {
		printf("Failed to read page. %d", GetLastError());
		return ERROR_ACCESS_DENIED;
	}

	if (BytesRead < ContentLength) {
		printf("Failed to read entire page. ContentLength: %d, BytesRead: %d",
			   ContentLength, BytesRead);
		return ERROR_ACCESS_DENIED;
	}

	PageChildrenEnumerate(Ctx, Page);

	return ERROR_SUCCESS;
}

UINT32
Scrape(_In_ LPCSTR Url)
{
	SESSION_CONTEXT Ctx = { 0 };
	UINT32 Ret;

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

	return Ret;
}