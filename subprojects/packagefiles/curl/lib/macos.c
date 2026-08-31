/* Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 * SPDX-License-Identifier: curl
 * clm-local overlay of curl's lib/macos.c, identical except for the
 * __has_include guard: an SDK-less cross toolchain (zig cc targeting
 * macOS) has no SystemConfiguration framework header. See curl.wrap. */

#include "curl_setup.h"

#ifdef CURL_MACOS_CALL_COPYPROXIES

#include <curl/curl.h>

#include "macos.h"

#if defined(__has_include) && \
  !__has_include(<SystemConfiguration/SCDynamicStoreCopySpecific.h>)

/* No framework headers: skip the SCDynamicStoreCopyProxies warm-up.
 * The only loss is IPv4-literal to IPv6 synthesis on NAT64 networks. */
CURLcode Curl_macos_init(void)
{
  return CURLE_OK;
}

#else

#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>

CURLcode Curl_macos_init(void)
{
  /*
   * The automagic conversion from IPv4 literals to IPv6 literals only
   * works if the SCDynamicStoreCopyProxies system function gets called
   * first. As curl currently does not support system-wide HTTP proxies, we
   * therefore do not use any value this function might return.
   */
  CFDictionaryRef dict = SCDynamicStoreCopyProxies(NULL);
  if(dict)
    CFRelease(dict);
  return CURLE_OK;
}

#endif

#endif
