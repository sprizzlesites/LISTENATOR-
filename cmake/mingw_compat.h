// Force-included first: only std headers, NEVER Windows headers.
// (Pulling <windows.h> here would precede JUCE's UNICODE setup and break all TCHAR types.)
#pragma once
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cwchar>
