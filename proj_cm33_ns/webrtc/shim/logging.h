/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Avnet
 *
 * Shim mapping upstream awslabs `examples/` log macros to our project's
 * stdout. Upstream's own logging.h pulls in `log_service.h` (a Realtek-
 * specific facility not present in our tree) and FreeRTOS task-name
 * decoration. We replace it with a minimal printf-backed implementation.
 *
 * Macro signature matches upstream: `LogError(( "fmt %d", arg ))` — the
 * double-paren form lets us forward the parenthesized printf argument
 * tuple as a single token.
 */
#ifndef WEBRTC_SHIM_LOGGING_H
#define WEBRTC_SHIM_LOGGING_H

#include <stdio.h>

#define LogError( msg )    do { printf( "[ERROR] " ); printf msg; printf( "\r\n" ); } while( 0 )
#define LogWarn( msg )     do { printf( "[WARN]  " ); printf msg; printf( "\r\n" ); } while( 0 )
#define LogInfo( msg )     do { printf( "[INFO]  " ); printf msg; printf( "\r\n" ); } while( 0 )
#define LogDebug( msg )    do { } while( 0 )
#define LogVerbose( msg )  do { } while( 0 )

#endif /* WEBRTC_SHIM_LOGGING_H */
