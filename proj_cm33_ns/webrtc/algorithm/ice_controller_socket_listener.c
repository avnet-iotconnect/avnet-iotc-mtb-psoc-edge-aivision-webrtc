/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdint.h>
#include "logging.h"
#include "ice_api.h"
#include "ice_controller.h"
#include "ice_controller_private.h"
#include "task.h"
#include "stun_deserializer.h"
#include "transport_mbedtls.h"

#if ENABLE_SCTP_DATA_CHANNEL
    #include "sctp_utils.h"
    #include "peer_connection_sctp.h"
#endif /* ENABLE_SCTP_DATA_CHANNEL */

#define ICE_CONTROLLER_SOCKET_LISTENER_SELECT_BLOCK_TIME_MS ( 50 )
#define RX_BUFFER_SIZE ( 4096 )

/* ── Raw-UART diagnostic helpers ────────────────────────────────────────── */
/* Used to surface TURN-unwrap failures that LogWarn/LogDebug can't because
 * the project's logging.h in this path is the AWS-KVS printf stub, not the
 * vLoggingPrintf-backed one.  Matches the pattern in ice_controller.c and
 * media_enc.c — spins on TXE bit 7 at USART1 ISR (0x56000C1C), writes TDR
 * at 0x56000C28, pets the watchdog on timeout.                             */
extern void vPetWatchdog( void );
static inline void isl_raw_putc( char c )
{
    for( uint32_t i = 0; i < 600000UL; i++ )
    {
        if( *(volatile uint32_t *)0x56000C1CUL & ( 1UL << 7 ) )
        {
            *(volatile uint32_t *)0x56000C28UL = ( uint32_t ) c;
            return;
        }
    }
    vPetWatchdog();
}
static void isl_raw_puts( const char *s ) { while( *s ) isl_raw_putc( *s++ ); }
static void isl_raw_dec( int v )
{
    char buf[ 12 ];
    int  n = 0;
    if( v < 0 ) { isl_raw_putc( '-' ); v = -v; }
    if( v == 0 ) { isl_raw_putc( '0' ); return; }
    while( v > 0 ) { buf[ n++ ] = '0' + ( v % 10 ); v /= 10; }
    while( n-- ) isl_raw_putc( buf[ n ] );
}
static void isl_raw_hex2( unsigned int v )
{
    static const char hex[] = "0123456789abcdef";
    isl_raw_putc( hex[ ( v >> 4 ) & 0xF ] );
    isl_raw_putc( hex[ v & 0xF ] );
}

static int32_t RecvPacketUdp( IceControllerSocketContext_t * pSocketContext,
                              uint8_t * pBuffer,
                              size_t bufferSize,
                              int flags,
                              IceEndpoint_t * pRemoteEndpoint )
{
    int32_t ret;
    struct sockaddr_storage srcAddress;
    socklen_t srcAddressLength = sizeof( srcAddress );
    struct sockaddr_in * pIpv4Address;
    struct sockaddr_in6 * pIpv6Address;
    uint8_t keepProcess = 1U;

    ret = recvfrom( pSocketContext->socketFd,
                    pBuffer,
                    bufferSize,
                    flags,
                    ( struct sockaddr * ) &srcAddress,
                    &srcAddressLength );

    /* UDP RX visibility for the W6x UDP-TURN diagnosis.  The ALLOCATING
     * timeout is ambiguous: either our Allocate request never left (TX broken)
     * or the server's response never came back (RX broken).  Emit:
     *   [isl] udpRx b=<N>  when any bytes actually arrive (includes port)
     *   [isl] udpErr e=<errno>  for errors other than EAGAIN/EWOULDBLOCK.
     * EAGAIN is omitted — it fires every select tick when the socket is idle. */
    if( ret > 0 )
    {
        uint16_t rxPort = 0;
        if( srcAddress.ss_family == AF_INET )
        {
            rxPort = ntohs( ( ( struct sockaddr_in * ) &srcAddress )->sin_port );
        }
        isl_raw_puts( "[isl] udpRx b=" );
        isl_raw_dec( ( int ) ret );
        isl_raw_puts( " p=" );
        isl_raw_dec( ( int ) rxPort );
        isl_raw_puts( "\r\n" );
    }
    else if( ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK )
    {
        isl_raw_puts( "[isl] udpErr e=" );
        isl_raw_dec( errno );
        isl_raw_puts( "\r\n" );
    }

    if( ret < 0 )
    {
        if( ( errno == EAGAIN ) || ( errno == EWOULDBLOCK ) )
        {
            /* Timeout, no more data to receive. */
            ret = 0;
            keepProcess = 0U;
        }
    }
    else if( ret == 0 )
    {
        /* Nothing to do if receive 0 byte. */
        keepProcess = 0U;
    }
    else
    {
        /* Empty else marker. */
    }

    if( keepProcess != 0U )
    {
        /* Received data, handle this STUN message. */
        if( srcAddress.ss_family == AF_INET )
        {
            pIpv4Address = ( struct sockaddr_in * ) &srcAddress;

            pRemoteEndpoint->transportAddress.family = STUN_ADDRESS_IPv4;
            pRemoteEndpoint->transportAddress.port = ntohs( pIpv4Address->sin_port );
            memcpy( pRemoteEndpoint->transportAddress.address, &pIpv4Address->sin_addr, STUN_IPV4_ADDRESS_SIZE );
        }
        else if( srcAddress.ss_family == AF_INET6 )
        {
            pIpv6Address = ( struct sockaddr_in6 * ) &srcAddress;

            pRemoteEndpoint->transportAddress.family = STUN_ADDRESS_IPv6;
            pRemoteEndpoint->transportAddress.port = ntohs( pIpv6Address->sin6_port );
            memcpy( pRemoteEndpoint->transportAddress.address, &pIpv6Address->sin6_addr, STUN_IPV6_ADDRESS_SIZE );
        }
        else
        {
            /* Unknown IP type, drop packet. */
            LogWarn( ( "Unknown source type(%d) from UDP connection.", srcAddress.ss_family ) );
            ret = -1;
        }
    }

    return ret;
}

static int32_t RecvPacketTls( IceControllerSocketContext_t * pSocketContext,
                              uint8_t * pBuffer,
                              size_t bufferSize,
                              IceEndpoint_t * pRemoteEndpoint )
{
    int32_t ret;

    memcpy( pRemoteEndpoint, &( pSocketContext->pIceServer->iceEndpoint ), sizeof( IceEndpoint_t ) );
    ret = TLS_FreeRTOS_recv( &pSocketContext->tlsSession.xTlsNetworkContext,
                             pBuffer,
                             bufferSize );

    if( ret < 0 )
    {
        LogError( ( "Receiving %ld from TLS connection", ret ) );
    }

    return ret;
}

static IceCandidatePair_t * FindCandidatePairByRemoteIceEndpoint( IceControllerContext_t * pCtx,
                                                                  IceControllerSocketContext_t * pSocketContext,
                                                                  IceEndpoint_t * pRemoteIceEndpoint )
{
    IceControllerResult_t result = ICE_CONTROLLER_RESULT_OK;
    IceCandidatePair_t * pCandidatePair = NULL;
    IceResult_t iceResult;
    size_t count;
    size_t i;
    uint8_t isLocked = 0U;

    /* Take ice lock. */
    if( xSemaphoreTake( pCtx->iceMutex, portMAX_DELAY ) == pdTRUE )
    {
        isLocked = 1U;
    }
    else
    {
        LogError( ( "Failed to process candidate pairs: mutex lock acquisition." ) );
        result = ICE_CONTROLLER_RESULT_FAIL_MUTEX_TAKE;
    }

    if( result == ICE_CONTROLLER_RESULT_OK )
    {
        iceResult = Ice_GetCandidatePairCount( &pCtx->iceContext,
                                               &count );
        if( iceResult != ICE_RESULT_OK )
        {
            LogError( ( "Fail to query valid candidate pair count, result: %d", iceResult ) );
            result = ICE_CONTROLLER_RESULT_FAIL_QUERY_CANDIDATE_PAIR_COUNT;
        }
    }

    if( result == ICE_CONTROLLER_RESULT_OK )
    {
        for( i = 0; i < count; i++ )
        {
            if( ( memcmp( &pCtx->iceContext.pCandidatePairs[i].pLocalCandidate->endpoint.transportAddress,
                          &pSocketContext->pLocalCandidate->endpoint.transportAddress,
                          sizeof( IceTransportAddress_t ) ) == 0 ) &&
                ( memcmp( &pCtx->iceContext.pCandidatePairs[i].pRemoteCandidate->endpoint.transportAddress,
                          &pRemoteIceEndpoint->transportAddress,
                          sizeof( IceTransportAddress_t ) ) == 0 ) )
            {
                pCandidatePair = &pCtx->iceContext.pCandidatePairs[i];
                break;
            }
        }
    }

    if( isLocked != 0U )
    {
        xSemaphoreGive( pCtx->iceMutex );
    }

    return pCandidatePair;
}

static IceControllerResult_t UpdateNominatedSocketContext( IceControllerContext_t * pCtx,
                                                           IceControllerSocketContext_t * pSocketContext,
                                                           IceCandidatePair_t * pCandidatePair,
                                                           IceEndpoint_t * pRemoteIceEndpoint )
{
    IceControllerResult_t ret = ICE_CONTROLLER_RESULT_OK;
    IceCandidatePair_t * pOriginalCandidatePair = NULL;
    OnIceEventCallback_t onIceEventCallbackFunc = NULL;
    void * pOnIceEventCallbackCustomContext = NULL;
    int32_t retPeerToPeerConnectionFound = 0;
    #if LIBRARY_LOG_LEVEL >= LOG_INFO
    char ipBuffer[ INET_ADDRSTRLEN ];
    #endif

    /* Find valid candidate pair pointer for current packet.
     * There are two scenarios:
     *   1. Host/Server Reflexive Candidates: The candidate pair pointer is NULL because we haven't mapped the remote endpoint.
     *   2. Peer/Client Reflexive Candidates: The candidate pair must be valid because we should extract it from Ice_HandleTurnPacket.
     */
    if( ( pSocketContext->pLocalCandidate->candidateType == ICE_CANDIDATE_TYPE_HOST ) ||
        ( pSocketContext->pLocalCandidate->candidateType == ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ) )
    {
        if( pCandidatePair == NULL )
        {
            pCandidatePair = FindCandidatePairByRemoteIceEndpoint( pCtx, pSocketContext, pRemoteIceEndpoint );
            if( pCandidatePair == NULL )
            {
                LogWarn( ( "Invalid to find candidate pair for the remote endpoint." ) );

                LogInfo( ( "Target remote endpoint IP address: %s, port: %u",
                           IceControllerNet_LogIpAddressInfo( pRemoteIceEndpoint, ipBuffer, sizeof( ipBuffer ) ),
                           pRemoteIceEndpoint->transportAddress.port ) );
                ret = ICE_CONTROLLER_RESULT_INVALID_PACKET;
            }
        }
        else
        {
            LogWarn( ( "ICE Candidate Pair Error: Host/Srflx candidate contains valid pair pointer when it should be NULL." ) );
            ret = ICE_CONTROLLER_RESULT_BAD_PARAMETER;
        }
    }
    else
    {
        /* Relay candidate. */
        if( pCandidatePair == NULL )
        {
            LogWarn( ( "ICE Candidate Pair Error: Relay candidate contains NULL pair pointer when it should be valid." ) );
            ret = ICE_CONTROLLER_RESULT_BAD_PARAMETER;
        }
    }

    if( ret == ICE_CONTROLLER_RESULT_OK )
    {
        /* Update nominated socket context. */
        if( xSemaphoreTake( pCtx->socketMutex, portMAX_DELAY ) == pdTRUE )
        {
            /* While running as viewer, the other master side might start DTLS handshaking
             * earlier than ICE_CANDIDATE STUN binding request. */
            if( pCtx->pNominatedSocketContext != NULL )
            {
                pOriginalCandidatePair = pCtx->pNominatedSocketContext->pCandidatePair;
            }
            pCtx->pNominatedSocketContext = pSocketContext;
            pCtx->pNominatedSocketContext->pRemoteCandidate = pCandidatePair->pRemoteCandidate;
            pCtx->pNominatedSocketContext->pCandidatePair = pCandidatePair;
            pCtx->pNominatedSocketContext->state = ICE_CONTROLLER_SOCKET_CONTEXT_STATE_SELECTED;

            onIceEventCallbackFunc = pCtx->onIceEventCallbackFunc;
            pOnIceEventCallbackCustomContext = pCtx->pOnIceEventCustomContext;

            ( void ) pOriginalCandidatePair;

            /* We have finished accessing the shared resource.  Release the mutex. */
            xSemaphoreGive( pCtx->socketMutex );

            LogInfo( ( "Nominated pair is changed from local/remote candidate ID: 0x%04x / 0x%04x to local/remote candidate ID: 0x%04x / 0x%04x",
                       pOriginalCandidatePair == NULL? 0:pOriginalCandidatePair->pLocalCandidate->candidateId,
                       pOriginalCandidatePair == NULL? 0:pOriginalCandidatePair->pRemoteCandidate->candidateId,
                       pCandidatePair->pLocalCandidate->candidateId,
                       pCandidatePair->pRemoteCandidate->candidateId ) );

            if( pOriginalCandidatePair == NULL )
            {
                IceController_UpdateState( pCtx, ICE_CONTROLLER_STATE_READY );
                IceController_UpdateTimerInterval( pCtx, ICE_CONTROLLER_PERIODIC_TIMER_INTERVAL_MS );

                /* Found nominated pair, execute DTLS handshake and release all other resources. */
                if( onIceEventCallbackFunc )
                {
                    retPeerToPeerConnectionFound = onIceEventCallbackFunc( pOnIceEventCallbackCustomContext,
                                                                           ICE_CONTROLLER_CB_EVENT_PEER_TO_PEER_CONNECTION_FOUND,
                                                                           NULL );
                    if( retPeerToPeerConnectionFound != 0 )
                    {
                        LogError( ( "Fail to handle peer to peer connection found event, ret: %ld", retPeerToPeerConnectionFound ) );
                    }
                }
                else
                {
                    LogWarn( ( "No callback function to handle P2P connection found event." ) );
                }
            }
        }
        else
        {
            LogError( ( "Failed to update nominated socket context: mutex lock acquisition." ) );
            ret = ICE_CONTROLLER_RESULT_FAIL_MUTEX_TAKE;
        }
    }

    return ret;
}

/* Determine the total on-wire length of a STUN message or TURN ChannelData
 * frame so we can split a TLS byte stream into individual messages.
 * Returns 0 if fewer than 4 bytes are available (need more data). */
static size_t GetStunTurnFrameLength( const uint8_t * pBuf, size_t bufLen )
{
    uint16_t payloadLen;
    size_t frameLen;

    if( bufLen < 4 )
    {
        return 0;
    }

    if( ( pBuf[ 0 ] & 0xC0 ) == 0x00 )
    {
        /* STUN message — top 2 bits are 0.
         * Bytes 2-3 = message length (excluding 20-byte header). */
        payloadLen = ( ( uint16_t ) pBuf[ 2 ] << 8 ) | pBuf[ 3 ];
        return ( size_t ) STUN_HEADER_LENGTH + payloadLen;
    }

    if( ( pBuf[ 0 ] & 0xF0 ) == 0x40 )
    {
        /* TURN ChannelData — first nibble 0x4.
         * Bytes 2-3 = data length.  Padded to 4-byte boundary on TCP. */
        payloadLen = ( ( uint16_t ) pBuf[ 2 ] << 8 ) | pBuf[ 3 ];
        frameLen = ( size_t ) ICE_TURN_CHANNEL_DATA_MESSAGE_HEADER_LENGTH + payloadLen;
        return ( frameLen + 3U ) & ~( size_t ) 3U;
    }

    /* Unknown frame type.  Return bufLen so the caller consumes (drops) the
     * whole buffer rather than looping forever. */
    LogWarn( ( "TLS frame: unrecognised first byte 0x%02x, dropping %u bytes",
               pBuf[ 0 ], ( unsigned ) bufLen ) );
    return bufLen;
}

/* Process a single complete frame through the relay-TURN check and the
 * RFC 5764 demux (STUN / DTLS / RTP).  Factored out of HandleRxPacket
 * so both the UDP path (one datagram = one frame) and the TLS path
 * (stream reassembly) can share the same logic. */
static IceControllerResult_t ProcessOneFrame( IceControllerContext_t * pCtx,
                                               IceControllerSocketContext_t * pSocketContext,
                                               uint8_t * pFrameBuffer,
                                               size_t frameLength,
                                               IceEndpoint_t * pRemoteIceEndpoint,
                                               OnRecvNonStunPacketCallback_t onRecvNonStunPacketFunc,
                                               void * pOnRecvNonStunPacketCallbackContext )
{
    IceControllerResult_t ret = ICE_CONTROLLER_RESULT_OK;
    IceResult_t iceResult;
    IceCandidatePair_t * pCandidatePair = NULL;
    uint8_t * pTurnPayload = NULL;
    uint16_t turnPayloadBufferLength = 0;
    uint8_t * pProcessingBuffer = pFrameBuffer;
    size_t processingBufferLength = frameLength;

    if( pSocketContext->pLocalCandidate->candidateType == ICE_CANDIDATE_TYPE_RELAY )
    {
        /* Demoted to Debug: under streaming this fires ~50 times/sec and
         * saturates the UART DMA, causing single-byte raw diagnostic
         * markers to be dropped by the TXE polling spin. */
        LogDebug( ( "RELAY RX %u bytes cand=0x%04x byte0=0x%02x",
                    ( unsigned ) frameLength,
                    pSocketContext->pLocalCandidate->candidateId,
                    pFrameBuffer[ 0 ] ) );

        if( xSemaphoreTake( pCtx->iceMutex, portMAX_DELAY ) == pdTRUE )
        {
            iceResult = Ice_HandleTurnPacket( &pCtx->iceContext,
                                              pProcessingBuffer,
                                              processingBufferLength,
                                              pSocketContext->pLocalCandidate,
                                              ( const uint8_t ** ) &pTurnPayload,
                                              &turnPayloadBufferLength,
                                              &pCandidatePair );
            xSemaphoreGive( pCtx->iceMutex );

            if( iceResult == ICE_RESULT_OK )
            {
                LogVerbose( ( "Removed TURN channel header for local/remote candidate ID 0x%04x / 0x%04x, number: 0x%02x%02x, length: 0x%02x%02x",
                              pCandidatePair->pLocalCandidate->candidateId,
                              pCandidatePair->pRemoteCandidate->candidateId,
                              pProcessingBuffer[ 0 ], pProcessingBuffer[ 1 ],
                              pProcessingBuffer[ 2 ], pProcessingBuffer[ 3 ] ) );

                /* Received TURN buffer, replace buffer pointer for further processing. */
                pProcessingBuffer = pTurnPayload;
                processingBufferLength = turnPayloadBufferLength;
            }
            else
            {
                /* TURN prefix not required, keep original buffer. */
                LogDebug( ( "No need to remove TURN header, result: %d", iceResult ) );

                /* Raw-UART diagnostic: when a ChannelData-looking packet
                 * (first byte 0x40-0x4F) reaches us on a RELAY socket but
                 * Ice_HandleTurnPacket refuses to strip it, that payload
                 * will fall through to the demux, hit the DTLS/STUN/RTP
                 * range checks, match none, and get dropped — silently
                 * breaking Chrome's DTLS handshake.  Print the iceResult
                 * and the first 4 bytes so we can tell which of the three
                 * failure cases (INVALID, UNEXPECTED, PAIR_NOT_FOUND) it
                 * is.  Only fires for channel-data-shaped bytes to keep
                 * the UART quiet during normal STUN traffic.               */
                if( ( processingBufferLength >= 4 ) &&
                    ( ( pProcessingBuffer[ 0 ] & 0xF0 ) == 0x40 ) )
                {
                    isl_raw_puts( "[isl] turnUnwrap FAIL r=" );
                    isl_raw_dec( ( int ) iceResult );
                    isl_raw_puts( " b=" );
                    isl_raw_hex2( pProcessingBuffer[ 0 ] );
                    isl_raw_hex2( pProcessingBuffer[ 1 ] );
                    isl_raw_hex2( pProcessingBuffer[ 2 ] );
                    isl_raw_hex2( pProcessingBuffer[ 3 ] );
                    isl_raw_puts( " len=" );
                    isl_raw_dec( ( int ) processingBufferLength );
                    isl_raw_puts( " cand=" );
                    isl_raw_hex2( ( pSocketContext->pLocalCandidate->candidateId >> 8 ) & 0xFF );
                    isl_raw_hex2( pSocketContext->pLocalCandidate->candidateId & 0xFF );
                    isl_raw_puts( " st=" );
                    isl_raw_dec( ( int ) pSocketContext->pLocalCandidate->state );
                    isl_raw_puts( "\r\n" );
                }
            }
        }
        else
        {
            LogError( ( "Failed to handle TURN packet: mutex lock acquisition." ) );
            return ICE_CONTROLLER_RESULT_FAIL_MUTEX_TAKE;
        }
    }

    /*
     *  demux each packet off of its first byte
     *  https://tools.ietf.org/html/rfc5764#section-5.1.2
     *  +----------------+
     *  | 127 < B < 192 -+--> forward to RTP/RTCP
     *  |                |
     *  |  19 < B < 64  -+--> forward to DTLS
     *  |                |
     *  |       B < 2   -+--> forward to STUN
     *  +----------------+
     */
    if( processingBufferLength > 0 )
    {
        if( ( ( pProcessingBuffer[ 0 ] > 127 ) && ( pProcessingBuffer[ 0 ] < 192 ) ) ||
            ( ( pProcessingBuffer[ 0 ] > 19 ) && ( pProcessingBuffer[ 0 ] < 64 ) ) )
        {
            if( onRecvNonStunPacketFunc )
            {
                if( pCtx->pNominatedSocketContext != pSocketContext )
                {
                    ret = UpdateNominatedSocketContext( pCtx, pSocketContext, pCandidatePair, pRemoteIceEndpoint );
                }

                if( ret == ICE_CONTROLLER_RESULT_OK )
                {
                    ( void ) onRecvNonStunPacketFunc( pOnRecvNonStunPacketCallbackContext,
                                                      pProcessingBuffer,
                                                      processingBufferLength );
                }
                else
                {
                    LogWarn( ( "DTLS packet rejected: Received from non-selected ICE candidate pair" ) );
                }
            }
            else
            {
                LogError( ( "No callback function to handle DTLS/RTP/RTCP packets." ) );
            }
        }
        else if( pProcessingBuffer[ 0 ] < 2 )
        {
            /* STUN packet. */
            ret = IceControllerNet_HandleStunPacket( pCtx,
                                                     pSocketContext,
                                                     pProcessingBuffer,
                                                     processingBufferLength,
                                                     pRemoteIceEndpoint,
                                                     pCandidatePair );
            if( ( ret == ICE_CONTROLLER_RESULT_FOUND_CONNECTION ) &&
                ( pCtx->pNominatedSocketContext == NULL ) )
            {
                UpdateNominatedSocketContext( pCtx,
                                              pSocketContext,
                                              pCandidatePair,
                                              pRemoteIceEndpoint );
            }
            else if( ( ret == ICE_CONTROLLER_RESULT_FOUND_CONNECTION ) || ( ret == ICE_CONTROLLER_RESULT_OK ) )
            {
                /* Handle STUN packet successfully. */
            }
            else if( ret == ICE_CONTROLLER_RESULT_CONNECTION_CLOSED )
            {
                /* Socket has been closed. */
            }
            else
            {
                LogError( ( "Fail to handle this RX packet, ret: %d, readBytes: %u", ret, ( unsigned ) processingBufferLength ) );
            }
        }
        else
        {
            LogWarn( ( "drop unknown packet, length=%u, first byte=0x%02x",
                       ( unsigned ) processingBufferLength,
                       pProcessingBuffer[ 0 ] ) );

            /* Raw-UART diagnostic mirror: LogWarn can be dropped by UART
             * backpressure.  Emit a compact line so we always know when
             * the demux threw a packet away, including the local cand
             * type + state so we can tell whether this was on the relay
             * (post-unwrap failure) or on a different socket entirely. */
            if( ( processingBufferLength >= 2 ) &&
                ( ( pProcessingBuffer[ 0 ] & 0xF0 ) == 0x40 ) )
            {
                isl_raw_puts( "[isl] drop b=" );
                isl_raw_hex2( pProcessingBuffer[ 0 ] );
                isl_raw_hex2( pProcessingBuffer[ 1 ] );
                isl_raw_puts( " len=" );
                isl_raw_dec( ( int ) processingBufferLength );
                isl_raw_puts( " candType=" );
                isl_raw_dec( ( int ) pSocketContext->pLocalCandidate->candidateType );
                isl_raw_puts( " st=" );
                isl_raw_dec( ( int ) pSocketContext->pLocalCandidate->state );
                isl_raw_puts( "\r\n" );
            }
        }
    }

    return ret;
}

static void HandleRxPacket( IceControllerContext_t * pCtx,
                            IceControllerSocketContext_t * pSocketContext,
                            OnRecvNonStunPacketCallback_t onRecvNonStunPacketFunc,
                            void * pOnRecvNonStunPacketCallbackContext )
{
    uint8_t skipProcess = 0;
    int32_t readBytes = 0;
    IceEndpoint_t remoteIceEndpoint;
    IceControllerResult_t ret = ICE_CONTROLLER_RESULT_OK;
    uint8_t receiveBuffer[ RX_BUFFER_SIZE ];
    size_t frameLen;

    if( ( pCtx == NULL ) || ( pSocketContext == NULL ) )
    {
        LogError( ( "Invalid input, pCtx: %p, pSocketContext: %p", pCtx, pSocketContext ) );
        skipProcess = 1;
    }

    /* ---- UDP path: one datagram = one complete message ---- */
    if( ( !skipProcess ) && ( pSocketContext->socketType == ICE_CONTROLLER_SOCKET_TYPE_UDP ) )
    {
        while( !skipProcess )
        {
            memset( &remoteIceEndpoint, 0, sizeof( IceEndpoint_t ) );
            readBytes = RecvPacketUdp( pSocketContext, receiveBuffer, RX_BUFFER_SIZE, 0, &remoteIceEndpoint );

            if( readBytes < 0 )
            {
                skipProcess = 1;
                break;
            }
            else if( readBytes == 0 )
            {
                break;
            }

            ret = ProcessOneFrame( pCtx, pSocketContext,
                                    receiveBuffer, ( size_t ) readBytes,
                                    &remoteIceEndpoint,
                                    onRecvNonStunPacketFunc,
                                    pOnRecvNonStunPacketCallbackContext );

            if( ret == ICE_CONTROLLER_RESULT_CONNECTION_CLOSED )
            {
                break;
            }
        }
    }
    /* ---- TLS path: byte-stream reassembly ---- */
    else if( ( !skipProcess ) && ( pSocketContext->socketType == ICE_CONTROLLER_SOCKET_TYPE_TLS ) )
    {
        /* Lazily allocate the per-socket reassembly buffer. */
        if( pSocketContext->pTlsRxBuf == NULL )
        {
            pSocketContext->pTlsRxBuf = ( uint8_t * ) pvPortMalloc( ICE_CONTROLLER_TLS_RX_BUF_SIZE );
            pSocketContext->tlsRxLen = 0;

            if( pSocketContext->pTlsRxBuf == NULL )
            {
                LogError( ( "Failed to allocate TLS RX reassembly buffer" ) );
                skipProcess = 1;
            }
        }

        while( !skipProcess )
        {
            /* 1. If no complete frame in the buffer, read more from TLS. */
            frameLen = GetStunTurnFrameLength( pSocketContext->pTlsRxBuf,
                                                pSocketContext->tlsRxLen );

            if( frameLen == 0 || frameLen > pSocketContext->tlsRxLen )
            {
                size_t space = ICE_CONTROLLER_TLS_RX_BUF_SIZE - pSocketContext->tlsRxLen;

                if( space == 0 )
                {
                    LogError( ( "TLS RX buffer full (%u bytes), draining",
                                ( unsigned ) pSocketContext->tlsRxLen ) );
                    pSocketContext->tlsRxLen = 0;
                    break;
                }

                readBytes = TLS_FreeRTOS_recv(
                    &pSocketContext->tlsSession.xTlsNetworkContext,
                    pSocketContext->pTlsRxBuf + pSocketContext->tlsRxLen,
                    space );

                if( readBytes < 0 )
                {
                    LogError( ( "TLS recv error %ld on fd %d",
                                ( long ) readBytes, pSocketContext->socketFd ) );
                    skipProcess = 1;
                    break;
                }
                else if( readBytes == 0 )
                {
                    break; /* No more data available right now. */
                }

                pSocketContext->tlsRxLen += ( size_t ) readBytes;

                /* Re-evaluate frame length after new data. */
                frameLen = GetStunTurnFrameLength( pSocketContext->pTlsRxBuf,
                                                    pSocketContext->tlsRxLen );

                if( frameLen == 0 || frameLen > pSocketContext->tlsRxLen )
                {
                    /* Still incomplete — wait for next poll cycle. */
                    break;
                }
            }

            /* 2. We have a complete frame of frameLen bytes.  Copy it into the
             *    stack buffer so the processing helpers can mutate it freely. */
            if( frameLen > RX_BUFFER_SIZE )
            {
                LogError( ( "TLS frame too large (%u bytes), dropping",
                            ( unsigned ) frameLen ) );
                /* Consume the oversized frame. */
                pSocketContext->tlsRxLen -= frameLen;
                if( pSocketContext->tlsRxLen > 0 )
                {
                    memmove( pSocketContext->pTlsRxBuf,
                             pSocketContext->pTlsRxBuf + frameLen,
                             pSocketContext->tlsRxLen );
                }
                continue;
            }

            memcpy( receiveBuffer, pSocketContext->pTlsRxBuf, frameLen );

            /* Consume the frame from the reassembly buffer. */
            pSocketContext->tlsRxLen -= frameLen;
            if( pSocketContext->tlsRxLen > 0 )
            {
                memmove( pSocketContext->pTlsRxBuf,
                         pSocketContext->pTlsRxBuf + frameLen,
                         pSocketContext->tlsRxLen );
            }

            /* 3. Set remote endpoint — TLS peer is always the TURN server. */
            memcpy( &remoteIceEndpoint,
                    &( pSocketContext->pIceServer->iceEndpoint ),
                    sizeof( IceEndpoint_t ) );

            /* 4. Process the complete frame. */
            ret = ProcessOneFrame( pCtx, pSocketContext,
                                    receiveBuffer, frameLen,
                                    &remoteIceEndpoint,
                                    onRecvNonStunPacketFunc,
                                    pOnRecvNonStunPacketCallbackContext );

            if( ret == ICE_CONTROLLER_RESULT_CONNECTION_CLOSED )
            {
                break;
            }
        }
    }
    else if( !skipProcess )
    {
        LogError( ( "Internal error, invalid socket type %d", pSocketContext->socketType ) );
    }

    if( readBytes < 0 )
    {
        /*
         * Socket read error detected (readBytes < 0).
         * This typically indicates the remote peer closed the connection.
         * Action required: Close the local socket to properly terminate the connection.
         */
        ( void ) Ice_CloseCandidate( &pCtx->iceContext, pSocketContext->pLocalCandidate );
        IceControllerNet_FreeSocketContext( pCtx, pSocketContext );
    }
}

static void pollingSockets( IceControllerContext_t * pCtx )
{
    fd_set rfds;
    int i;
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = ICE_CONTROLLER_SOCKET_LISTENER_SELECT_BLOCK_TIME_MS * 1000,
    };
    int maxFd = 0;
    int retSelect;
    uint8_t skipProcess = 0;
    int fds[ ICE_CONTROLLER_MAX_LOCAL_CANDIDATE_COUNT ];
    size_t fdsCount;
    OnRecvNonStunPacketCallback_t onRecvNonStunPacketFunc;
    void * pOnRecvNonStunPacketCallbackContext = NULL;
    IceControllerSocketContext_t * pSocketContext;

    FD_ZERO( &rfds );

    if( xSemaphoreTake( pCtx->socketMutex, portMAX_DELAY ) == pdTRUE )
    {
        for( i = 0; i < pCtx->socketsContextsCount; i++ )
        {
            fds[i] = pCtx->socketsContexts[i].socketFd;
        }
        fdsCount = pCtx->socketsContextsCount;
        onRecvNonStunPacketFunc = pCtx->socketListenerContext.onRecvNonStunPacketFunc;
        pOnRecvNonStunPacketCallbackContext = pCtx->socketListenerContext.pOnRecvNonStunPacketCallbackContext;

        /* We have finished accessing the shared resource.  Release the mutex. */
        xSemaphoreGive( pCtx->socketMutex );
    }
    else
    {
        LogError( ( "Unexpected behavior: fail to take mutex" ) );
        skipProcess = 1;
    }

    if( !skipProcess )
    {
        /* Set rfds for select function. */
        for( i = 0; i < fdsCount; i++ )
        {
            /* fds might be removed for any reason. Handle that by checking if it's -1. */
            if( fds[i] >= 0 )
            {
                FD_SET( fds[i], &rfds );
                if( fds[i] > maxFd )
                {
                    maxFd = fds[i];
                }
            }
        }

        /* Poll all socket handlers. */
        retSelect = select( maxFd + 1, &rfds, NULL, NULL, &tv );
        if( retSelect < 0 )
        {
            LogError( ( "select return error value %d", retSelect ) );
            skipProcess = 1;
        }
        else if( retSelect == 0 )
        {
            /* It's just timeout. */
            skipProcess = 1;
        }
        else
        {
            /* Empty else marker. */
        }
    }

    if( !skipProcess )
    {
        for( i = 0; i < fdsCount; i++ )
        {
            if( ( fds[i] >= 0 ) && FD_ISSET( fds[i], &rfds ) )
            {
                pSocketContext = &( pCtx->socketsContexts[ i ] );

                if( pSocketContext->state == ICE_CONTROLLER_SOCKET_CONTEXT_STATE_CONNECTION_IN_PROGRESS )
                {
                    ( void ) IceControllerNet_ExecuteTlsHandshake( pCtx, pSocketContext, 0U );
                }
                else
                {
                    HandleRxPacket( pCtx,
                                    pSocketContext,
                                    onRecvNonStunPacketFunc,
                                    pOnRecvNonStunPacketCallbackContext );
                }
            }
        }
    }
}

IceControllerResult_t IceControllerSocketListener_StartPolling( IceControllerContext_t * pCtx )
{
    IceControllerResult_t ret = ICE_CONTROLLER_RESULT_OK;

    if( xSemaphoreTake( pCtx->socketMutex, portMAX_DELAY ) == pdTRUE )
    {
        pCtx->socketListenerContext.executeSocketListener = 1;

        /* We have finished accessing the shared resource.  Release the mutex. */
        xSemaphoreGive( pCtx->socketMutex );

        LogDebug( ( "Socket Listener: start polling" ) );
    }
    else
    {
        LogError( ( "Unexpected behavior: fail to take mutex" ) );
        ret = ICE_CONTROLLER_RESULT_FAIL_MUTEX_TAKE;
    }

    return ret;
}

IceControllerResult_t IceControllerSocketListener_StopPolling( IceControllerContext_t * pCtx )
{
    IceControllerResult_t ret = ICE_CONTROLLER_RESULT_OK;

    if( xSemaphoreTake( pCtx->socketMutex, portMAX_DELAY ) == pdTRUE )
    {
        pCtx->socketListenerContext.executeSocketListener = 0;

        /* We have finished accessing the shared resource.  Release the mutex. */
        xSemaphoreGive( pCtx->socketMutex );

        LogDebug( ( "Socket Listener: stop polling" ) );
    }
    else
    {
        LogError( ( "Unexpected behavior: fail to take mutex" ) );
        ret = ICE_CONTROLLER_RESULT_FAIL_MUTEX_TAKE;
    }

    return ret;
}

IceControllerResult_t IceControllerSocketListener_Init( IceControllerContext_t * pCtx,
                                                        OnRecvNonStunPacketCallback_t onRecvNonStunPacketFunc,
                                                        void * pOnRecvNonStunPacketCallbackContext )
{
    IceControllerResult_t ret = ICE_CONTROLLER_RESULT_OK;

    if( pCtx == NULL )
    {
        LogError( ( "Invalid input: pCtx is NULL" ) );
        ret = ICE_CONTROLLER_RESULT_BAD_PARAMETER;
    }

    if( ret == ICE_CONTROLLER_RESULT_OK )
    {
        pCtx->socketListenerContext.executeSocketListener = 0;
        pCtx->socketListenerContext.onRecvNonStunPacketFunc = onRecvNonStunPacketFunc;
        pCtx->socketListenerContext.pOnRecvNonStunPacketCallbackContext = pOnRecvNonStunPacketCallbackContext;
    }

    return ret;
}

void IceControllerSocketListener_Task( void * pParameter )
{
    IceControllerContext_t * pCtx = ( IceControllerContext_t * ) pParameter;

    for( ;; )
    {
        while( pCtx->socketListenerContext.executeSocketListener == 0 )
        {
            vTaskDelay( pdMS_TO_TICKS( ICE_CONTROLLER_SOCKET_LISTENER_SELECT_BLOCK_TIME_MS ) );
        }

        if( pCtx->socketListenerContext.executeSocketListener == 1 )
        {
            pollingSockets( pCtx );
        }

        /* Pet watchdog — this task polls sockets continuously during ICE
         * and can starve the idle hook that normally refreshes the IWDG. */
        vPetWatchdog();
    }
}
