/* SPDX-License-Identifier: MIT
 * Copyright (C) 2025 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com>, Shu Liu <shu.liu@avnet.com> et al.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "iotconnect.h"

// see IotConnectConnectionType: IOTC_CT_AWS or IOTC_CT_AZURE
#define IOTCONNECT_CONNECTION_TYPE IOTC_CT_AWS
#define IOTCONNECT_CPID "97FF86E8728645E9B89F7B07977E4B15"
#define IOTCONNECT_ENV  "poc"

// If Device Unique ID (DUID) is not provided, a generated DUID will be used using the below prefix
#define IOTCONNECT_DUID "nik-e84-webrtc" 
// prefix for the auto-generated name based on chip HWUID that will be used if IOTCONNECT_DUID is not supplied
#define IOTCONNECT_DUID_PREFIX "psoc-edge-rm-"

/*
 PEM format certificate and private key
Example:
#define IOTCONNECT_DEVICE_CERT \
"-----BEGIN CERTIFICATE-----\n" \
".... base64 encoded certificate ..."\
"-----END CERTIFICATE-----"
#define IOTCONNECT_DEVICE_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
".... base64 encoded private key ..."\
"-----END PRIVATE KEY-----"
Leave certificate and private key blank if  you wish to use EMEEPROM data to automatically generate the certificate
and use the runtime configuration for all of the configurable values in this file.
IMPORTANT NOTE: If you use the EMEEPROM runtime configuration,
the certificate and private key will be regenerated when you re-flash the board!
In that case, you would need to delete and re-create your device in IoTConnect.
*/
#define IOTCONNECT_DEVICE_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDWTCCAkGgAwIBAgIUYQJY2ApKGHsMvIUbNbGPiWSU6jkwDQYJKoZIhvcNAQEL\n" \
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\n" \
"InMuBExlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI2MDQyMzE1NTgwNloX\n" \
"DTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0ZTCC\n" \
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALKQto8MueJtGewG/rys\n" \
"gE2x1ibuOE3mz+irWJswyvd//ckACDZzVK5vAkBKgRuvEc/5Td6PFM3aUlmvUmYV\n" \
"Xp+NjWe9OWS03OvoC4wDDRR99Y1/7ci9QbcuiSlN6VrcWDSXa/CHBeGt9Hvw4Kh9\n" \
"CLVZg8yZRkbrfobyBLv5P32q0pc/OLokXoJOqYmCpKPfaoVxJex9uArJXyqgRIYX\n" \
"XEMHqc+Tm0HBWXH1kX+rFHFsEd8Im0mgIFs53TjO8jW8gnBmc12tst2w6mJmiDR+\n" \
"uan9hM4jPwvbhnYYJSbBPaDwpCdXLYpb9cuAem5gILFdXFTPo/W6gPDvP8MXLzh4\n" \
"X/0CAwEAAaNgMF4wHwYDVR0jBBgwFoAUq9wSF7pcwtLRTiIK5jk1OKkjzwAwHQYD\n" \
"VR0OBBYEFMR5tkRtpU4U77Dm+ixalXWmSiOVMAwGA1UdEwEB/wQCMAAwDgYDVR0P\n" \
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQCuux4lXYvc2PPq3fuGT6IJbqs6\n" \
"Mt33lsCeDEz5oygtQ8SjslC6f0Vc2QLCHw/nfdRSFn6gYSIfFWTf8Hq3bRoWLvtm\n" \
"THHTqrCuBIxA+r1PgT/nQcTih72PYUzfXm6PqgiCcNSk3ZJJmqQ81Pb9M/U1TojY\n" \
"V+QQiSNqcPZSlvP1eeL6xQw38JKoC4wFHEukrBZPiydxdG4EyiBvEtOY1wmThwy1\n" \
"ky4h4X0bAQwn7qRzkS99MA1UwinsQzgzGm3T2VvzNGp8EWOfxs0EOhI3iqwJ+q90\n" \
"qGAJpjV2zojhj+l0ba1v6FZv8qTCaUFb86U7QpokoiLeb9+EQw+JTAB4wYPZ\n" \
"-----END CERTIFICATE-----\n"

#define IOTCONNECT_DEVICE_KEY \
"-----BEGIN RSA PRIVATE KEY-----\n" \
"MIIEowIBAAKCAQEAspC2jwy54m0Z7Ab+vKyATbHWJu44TebP6KtYmzDK93/9yQAI\n" \
"NnNUrm8CQEqBG68Rz/lN3o8UzdpSWa9SZhVen42NZ705ZLTc6+gLjAMNFH31jX/t\n" \
"yL1Bty6JKU3pWtxYNJdr8IcF4a30e/DgqH0ItVmDzJlGRut+hvIEu/k/farSlz84\n" \
"uiRegk6piYKko99qhXEl7H24CslfKqBEhhdcQwepz5ObQcFZcfWRf6sUcWwR3wib\n" \
"SaAgWzndOM7yNbyCcGZzXa2y3bDqYmaINH65qf2EziM/C9uGdhglJsE9oPCkJ1ct\n" \
"ilv1y4B6bmAgsV1cVM+j9bqA8O8/wxcvOHhf/QIDAQABAoIBADVSRVF+KVvV3HHA\n" \
"vPy2PjH1Ms+5PxL0I42E7RaagWFa1PvaiJlJ4zWbbH6qQNd1dco21evpqfq2gPTL\n" \
"B734D6Nxb5JUzPinkXjYd+o9VwCRhGvWkbjp2t/Kg6bT3yUEu0opFVBj4Xu8qE8C\n" \
"ZPOrMpiEvHiyJGw1EjlVt9gx26W4hyofVUSNUJokR+XwRoETxMbHIuw9wSemaqae\n" \
"aeJUDf8V9dzqCfrhAMMOC2DKzMYnKgwY5WwzpP+WYJJHUXYVRNEXVrpIZCXw4Tym\n" \
"/qcz/wxutATZR3dznnoEYyxrvMt6GMJIsBHbCcCC4ZmuF028QzhqpcZcuJjScmJD\n" \
"7rSmn3kCgYEA2NGQ9nwNq0mHVHJJ9C2hCqVJ9hV7Uo2KlVnmGCQjUF+J1efJ0QHP\n" \
"6TnD7uVObzRoTfFpKEPprAZ9ocAirLgPmCfCzRFK2A2KFI9yojBBsCIfnsYQ+/O5\n" \
"+9+Xd40B2qehIUVTaJlOy0HE7EWLEUeIzSTh4BOK3/O8PmVmDVCw1PsCgYEA0tV5\n" \
"8RaF+vC3MInxE6cByLpMDcIfcdiBcknLGAtvSy/cQw70bFcwT8BH12oC4zshvrST\n" \
"hoZxwx3Gq0gDG7DrEMOZ105BVb8N3F7rRiKNYQMzNWNjOxrqjHnytHHPzP6cI1vO\n" \
"8U5byiKXgH7OZu3gynZpiZSOMZha90IItjaC3WcCgYEAyrmfviedrEMyys2Vwj6L\n" \
"reWAMxxA055OLnkBAB+B+VtrCFsSQ19bY+lD/vHelXG9+Oq49RISwWrMOtnoUfBD\n" \
"fHPo207I2RxHuBOsDmPqd2JgiFcPeFSZ2BxmhjyMuEI2EWK9qzZ1Cu0yz+vyLLTi\n" \
"/1T18Uq0ddNydjDe66I/xNMCgYAeNGE07eIh8kx8UrbL9vgldrMgoXf5yme5JONI\n" \
"vZKjNecbYFAaGV+dfVGNhe2F+lm6RYqBEk/IAUMOFNIFLJJwo2Ut2FM54pYsxRh1\n" \
"wst4Y4n5kxSLSi7iEzJ3MXrwxmU+F5ANOAmkatJDoWcWjevPdalnAXZhmo8lrzsq\n" \
"R6L17wKBgAaQO/NmrGtLZp3xMFFCzu0gloFyjuOFPAScZYFv/gmV9E2fgHmN+bhG\n" \
"4+gultsxLC19qNoE3AzOANg3OtIiueHygZpdN9C2dQCeN2G3Zmi/0eXCCbUAw8Wu\n" \
"7g6bAaFfqOb8xDp59zosxHIaob75Nt5Zkl1K0Gad+LKLiLZo11fx\n" \
"-----END RSA PRIVATE KEY-----\n"


// you can choose to use your own NTP server to obtain network time, or simply time.google.com for better stability
#define IOTCONNECT_SNTP_SERVER "pool.ntp.org"

#endif /* APP_CONFIG_H */
