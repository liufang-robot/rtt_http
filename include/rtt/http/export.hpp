#pragma once

#if defined(_WIN32)
#if defined(RTT_HTTP_BUILD)
#define RTT_HTTP_API __declspec(dllexport)
#else
#define RTT_HTTP_API __declspec(dllimport)
#endif
#else
#define RTT_HTTP_API __attribute__((visibility("default")))
#endif
