#ifndef PARSE_H
#define PARSE_H

#include <Windows.h>
#include <wininet.h>
#include <stdio.h>

#pragma comment(lib, "Wininet.lib")

#define STATIC static

UINT32
Scrape(_In_ LPCSTR Url);

#endif