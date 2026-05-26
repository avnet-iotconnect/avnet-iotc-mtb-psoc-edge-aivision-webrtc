/* Standard includes. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Interface includes. */
#include "sdp_serializer.h"

/* fork-aivision: newlib-nano on PSE84 omits %llu/%lld from printf. Pre-format
 * uint64 fields to a stack buffer and splice via %.*s. Keeps newlib-nano. */
#define U64_DEC_MAX 21  /* 20 digits for 2^64-1 + NUL */

static size_t u64_to_dec( uint64_t v, char * out )
{
    char tmp[ U64_DEC_MAX ];
    size_t i = 0, n;
    if( v == 0U )
    {
        out[ 0 ] = '0';
        return 1U;
    }
    while( v > 0U )
    {
        tmp[ i++ ] = ( char ) ( '0' + ( v % 10U ) );
        v /= 10U;
    }
    n = i;
    while( i > 0U )
    {
        i--;
        *out++ = tmp[ i ];
    }
    return n;
}

SdpResult_t SdpSerializer_Init( SdpSerializerContext_t * pCtx,
                                char * pBuffer,
                                size_t bufferLength )
{
    SdpResult_t result = SDP_RESULT_OK;

    if( pCtx == NULL )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        pCtx->pStart = pBuffer;
        pCtx->totalLength = bufferLength;
        pCtx->currentIndex = 0;
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddBuffer( SdpSerializerContext_t * pCtx,
                                     uint8_t type,
                                     const char * pValue,
                                     size_t valueLength )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( pValue == NULL ) ||
        ( valueLength == 0 ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        snprintfRetVal = snprintf( pWriteBuffer,
                                   remainingLength,
                                   "%c=%.*s\r\n",
                                   type,
                                   ( int ) valueLength, pValue );
        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddU32( SdpSerializerContext_t * pCtx,
                                  uint8_t type,
                                  uint32_t value )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        snprintfRetVal = snprintf( pWriteBuffer,
                                   remainingLength,
                                   "%c"
                                   "=%" SDP_PRINT_FMT_UINT32 "\r\n",
                                   type,
                                   value );
        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddU64( SdpSerializerContext_t * pCtx,
                                  uint8_t type,
                                  uint64_t value )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        {
            char valBuf[ U64_DEC_MAX ];
            size_t valLen = u64_to_dec( value, valBuf );
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c"
                                       "=%.*s\r\n",
                                       type,
                                       ( int ) valLen, valBuf );
        }
        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddOriginator( SdpSerializerContext_t * pCtx,
                                         uint8_t type,
                                         const SdpOriginator_t * pOriginator )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pOriginator == NULL ) ||
        ( pOriginator->pUserName == NULL ) ||
        ( pOriginator->connectionInfo.pAddress == NULL ) ||
        ( pOriginator->connectionInfo.networkType != SDP_NETWORK_IN ) ||
        ( ( pOriginator->connectionInfo.addressType != SDP_ADDRESS_IPV4 ) &&
          ( pOriginator->connectionInfo.addressType != SDP_ADDRESS_IPV6 ) ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        {
            char sidBuf[ U64_DEC_MAX ], svBuf[ U64_DEC_MAX ];
            size_t sidLen = u64_to_dec( pOriginator->sessionId, sidBuf );
            size_t svLen  = u64_to_dec( pOriginator->sessionVersion, svBuf );
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c"
                                       "=%.*s"
                                       " %.*s"
                                       " %.*s"
                                       " %.*s"
                                       " %.*s"
                                       " %.*s\r\n",
                                       type,
                                       ( int ) pOriginator->userNameLength, pOriginator->pUserName,
                                       ( int ) sidLen, sidBuf,
                                       ( int ) svLen, svBuf,
                                       2, "IN",
                                       3, pOriginator->connectionInfo.addressType == SDP_ADDRESS_IPV4 ? "IP4" : "IP6",
                                       ( int ) pOriginator->connectionInfo.addressLength, pOriginator->connectionInfo.pAddress );
        }

        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddConnectionInfo( SdpSerializerContext_t * pCtx,
                                             uint8_t type,
                                             const SdpConnectionInfo_t * pConnInfo )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pConnInfo == NULL ) ||
        ( pConnInfo->networkType != SDP_NETWORK_IN ) ||
        ( pConnInfo->pAddress == NULL ) ||
        ( ( pConnInfo->addressType != SDP_ADDRESS_IPV4 ) &&
          ( pConnInfo->addressType != SDP_ADDRESS_IPV6 ) ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        snprintfRetVal = snprintf( pWriteBuffer,
                                   remainingLength,
                                   "%c=%.*s %.*s %.*s\r\n",
                                   type,
                                   2, "IN",
                                   3, pConnInfo->addressType == SDP_ADDRESS_IPV4 ? "IP4" : "IP6",
                                   ( int ) pConnInfo->addressLength, pConnInfo->pAddress );

        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddBandwidthInfo( SdpSerializerContext_t * pCtx,
                                            uint8_t type,
                                            const SdpBandwidthInfo_t * pBandwidthInfo )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pBandwidthInfo == NULL ) ||
        ( pBandwidthInfo->pBwType == NULL ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        {
            char bwBuf[ U64_DEC_MAX ];
            size_t bwLen = u64_to_dec( pBandwidthInfo->sdpBandwidthValue, bwBuf );
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c=%.*s"
                                       ":%.*s\r\n",
                                       type,
                                       ( int ) pBandwidthInfo->bwTypeLength, pBandwidthInfo->pBwType,
                                       ( int ) bwLen, bwBuf );
        }

        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddTimeActive( SdpSerializerContext_t * pCtx,
                                         uint8_t type,
                                         const SdpTimeDescription_t * pTimeDescription )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pTimeDescription == NULL ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        {
            char startBuf[ U64_DEC_MAX ], stopBuf[ U64_DEC_MAX ];
            size_t startLen = u64_to_dec( pTimeDescription->startTime, startBuf );
            size_t stopLen  = u64_to_dec( pTimeDescription->stopTime, stopBuf );
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c"
                                       "=%.*s"
                                       " %.*s\r\n",
                                       type,
                                       ( int ) startLen, startBuf,
                                       ( int ) stopLen, stopBuf );
        }

        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddAttribute( SdpSerializerContext_t * pCtx,
                                        uint8_t type,
                                        const SdpAttribute_t * pAttribute )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pAttribute == NULL ) ||
        ( pAttribute->pAttributeName == NULL ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        if( pAttribute->pAttributeValue != NULL )
        {
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c=%.*s:%.*s\r\n",
                                       type,
                                       ( int ) pAttribute->attributeNameLength, pAttribute->pAttributeName,
                                       ( int ) pAttribute->attributeValueLength, pAttribute->pAttributeValue );
        }
        else
        {
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c=%.*s\r\n",
                                       type,
                                       ( int ) pAttribute->attributeNameLength, pAttribute->pAttributeName );
        }

        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_AddMedia( SdpSerializerContext_t * pCtx,
                                    uint8_t type,
                                    const SdpMedia_t * pMedia )
{
    int snprintfRetVal;
    SdpResult_t result = SDP_RESULT_OK;
    size_t remainingLength = 0;
    char * pWriteBuffer = NULL;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pMedia == NULL ) ||
        ( pMedia->pProtocol == NULL ) ||
        ( pMedia->pFmt == NULL ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        if( pCtx->pStart != NULL )
        {
            pWriteBuffer = &( pCtx->pStart[ pCtx->currentIndex ] );
            remainingLength = pCtx->totalLength - pCtx->currentIndex;
        }

        if( pMedia->portNum != 0 )
        {
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c"
                                       "=%.*s"
                                       " %" SDP_PRINT_FMT_UINT16
                                       "/%" SDP_PRINT_FMT_UINT16
                                       " %.*s"
                                       " %.*s\r\n",
                                       type,
                                       ( int ) pMedia->mediaLength, pMedia->pMedia,
                                       pMedia->port,
                                       pMedia->portNum,
                                       ( int ) pMedia->protocolLength, pMedia->pProtocol,
                                       ( int ) pMedia->fmtLength, pMedia->pFmt );
        }
        else
        {
            snprintfRetVal = snprintf( pWriteBuffer,
                                       remainingLength,
                                       "%c"
                                       "=%.*s"
                                       " %" SDP_PRINT_FMT_UINT16
                                       " %.*s"
                                       " %.*s\r\n",
                                       type,
                                       ( int ) pMedia->mediaLength, pMedia->pMedia,
                                       pMedia->port,
                                       ( int ) pMedia->protocolLength, pMedia->pProtocol,
                                       ( int ) pMedia->fmtLength, pMedia->pFmt );
        }

        /* LCOV_EXCL_START */
        if( snprintfRetVal < 0 )
        {
            result = SDP_RESULT_SNPRINTF_ERROR;
        }
        /* LCOV_EXCL_STOP  */
        else if( ( pWriteBuffer != NULL ) && ( ( size_t ) snprintfRetVal >= remainingLength ) )
        {
            result = SDP_RESULT_OUT_OF_MEMORY;
        }
        else
        {
            pCtx->currentIndex += ( size_t ) snprintfRetVal;
        }
    }

    return result;
}
/*-----------------------------------------------------------*/

SdpResult_t SdpSerializer_Finalize( SdpSerializerContext_t * pCtx,
                                    const char ** pSdpMessage,
                                    size_t * pSdpMessageLength )
{
    SdpResult_t result = SDP_RESULT_OK;

    if( ( pCtx == NULL ) ||
        ( ( pCtx->pStart != NULL ) &&
          ( pCtx->currentIndex > pCtx->totalLength ) ) ||
        ( pSdpMessage == NULL ) ||
        ( pSdpMessageLength == NULL ) )
    {
        result = SDP_RESULT_BAD_PARAM;
    }

    if( result == SDP_RESULT_OK )
    {
        *pSdpMessage = pCtx->pStart;
        *pSdpMessageLength = pCtx->currentIndex;
    }

    return result;
}
/*-----------------------------------------------------------*/
