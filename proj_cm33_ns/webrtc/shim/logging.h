/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Avnet
 *
 */
 
#ifndef WEBRTC_SHIM_LOGGING_H
#define WEBRTC_SHIM_LOGGING_H

#include <stdio.h>

/* Log levels, ascending verbosity. Match upstream's numbering. */
#define LOG_NONE       0
#define LOG_ERROR      1
#define LOG_WARN       2
#define LOG_INFO       3
#define LOG_DEBUG      4
#define LOG_VERBOSE    5

/*
 * Active threshold: macros at or below this level emit, the rest compile out.
 *
 * WARNING: treat this as fixed at LOG_INFO. The upstream awslabs `examples/`
 * sources are not clean across log levels -- they declare log-only scratch
 * variables (e.g. ipBuffer, rxIp/rxPort) that are read solely inside LogDebug/
 * LogVerbose calls, sometimes without a matching `#if LIBRARY_LOG_LEVEL >= ...`
 * guard. Lowering this drops their only consumer and trips -Werror-class
 * unused-variable diagnostics; raising it pulls in verbose buffers and per-
 * packet spam. Changing it means auditing those call sites by hand. Don't.
 */
#define LIBRARY_LOG_LEVEL    LOG_INFO

#if LIBRARY_LOG_LEVEL >= LOG_ERROR
#define LogError( msg )    do { printf( "[ERROR] " ); printf msg; printf( "\r\n" ); } while( 0 )
#else
#define LogError( msg )    do { } while( 0 )
#endif

#if LIBRARY_LOG_LEVEL >= LOG_WARN
#define LogWarn( msg )     do { printf( "[WARN]  " ); printf msg; printf( "\r\n" ); } while( 0 )
#else
#define LogWarn( msg )     do { } while( 0 )
#endif

#if LIBRARY_LOG_LEVEL >= LOG_INFO
#define LogInfo( msg )     do { printf( "[INFO]  " ); printf msg; printf( "\r\n" ); } while( 0 )
#else
#define LogInfo( msg )     do { } while( 0 )
#endif

#if LIBRARY_LOG_LEVEL >= LOG_DEBUG
#define LogDebug( msg )    do { printf( "[DEBUG] " ); printf msg; printf( "\r\n" ); } while( 0 )
#else
#define LogDebug( msg )    do { } while( 0 )
#endif

#if LIBRARY_LOG_LEVEL >= LOG_VERBOSE
#define LogVerbose( msg )  do { printf( "[VERB]  " ); printf msg; printf( "\r\n" ); } while( 0 )
#else
#define LogVerbose( msg )  do { } while( 0 )
#endif

#endif /* WEBRTC_SHIM_LOGGING_H */
