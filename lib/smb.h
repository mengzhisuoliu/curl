#ifndef HEADER_CURL_SMB_H
#define HEADER_CURL_SMB_H
/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Bill Nagel <wnagel@tycoint.com>, Exacq Technologies
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#if !defined(CURL_DISABLE_SMB) && defined(USE_CURL_NTLM_CORE) && \
  (SIZEOF_CURL_OFF_T > 4)

extern const struct Curl_handler Curl_handler_smb;
extern const struct Curl_handler Curl_handler_smbs;

#endif /* CURL_DISABLE_SMB && USE_CURL_NTLM_CORE && SIZEOF_CURL_OFF_T > 4 */

#endif /* HEADER_CURL_SMB_H */
