## Avnet PSOC™ Edge AI Vision WebRTC

This demo project is the integration of Infineon's 
[PSOC™ Edge MCU: Machine learning – DEEPCRAFT™ deploy vision](https://github.com/Infineon/mtb-example-psoc-edge-ml-deepcraft-deploy-vision/tree/release-v2.2.0)
and [Avnet /IOTCONNECT ModusToolbox&trade; SDK](https://github.com/avnet-iotconnect/avnet-iotc-mtb-sdk)
with /IOTCONNECT Amazon Kinesis WebRTC support.

This project has a three project structure: CM33 secure, CM33 non-secure, and CM55 projects.
All three projects are programmed to the external QSPI flash and executed in Execute in Place (XIP) mode. 
Extended boot launches the CM33 secure project from a fixed location in the external flash, which then configures the protection settings and launches the CM33 non-secure application. Additionally, CM33 non-secure application enables CM55 CPU and launches the CM55 application.

The M55 processor performs H.264 video encoding, LCD and the DEEPCRAFT™ model heavy lifting.
It transmits the H.264 frames to CM33 and reports inference results via IPC to the M33 processor.
The M33 Non-Secure application is a custom /IOTCONNECT application that is receiving the IPC messages,
managing the /IOTCONNECT WebRTC connection, processing the data and sending it to /IOTCONNECT. 
This application can receive Cloud-To-Device commands as well and control one of the board LEDs or control the application flow.    

## Requirements

- [ModusToolbox&trade;](https://www.infineon.com/modustoolbox) with MTB Tools v3.7 or later (tested with v3.7)
- Board support package (BSP) minimum required version: 1.0.0
- Programming language: C
- Associated parts: All [PSOC&trade; Edge MCU](https://www.infineon.com/products/microcontroller/32-bit-psoc-arm-cortex/32-bit-psoc-edge-arm) parts
- Recommended: [Waveshare 4.3 inch Raspberry Pi DSI 800*480 display](https://www.waveshare.com/4.3inch-dsi-lcd.htm)

## Supported toolchains (make variable 'TOOLCHAIN')

- GNU Arm&reg; Embedded Compiler v14.2.1 (`GCC_ARM`) – Default value of `TOOLCHAIN`
- LLVM Embedded Toolchain for Arm® v19.1.5 (LLVM_ARM)
> **Note:**
> * This code example fails to build in RELEASE mode with the GCC_ARM toolchain v14.2.1 as it does not recognize some of the Helium instructions of the CMSIS-DSP library. This issue is not present in the Arm® Compiler for Embedded (armclang)
> * This code example currently supports the VFP_SELECT option "hardfp" only. Setting "softfp" may cause build failure.
> * While LLVM will compile and run the project, a severe H.264 encoding performance degradation is observed with the LLVM toolchain compared to GCC.

## Supported kits (make variable 'TARGET')

- [PSOC&trade; Edge E84 AI Kit](https://www.infineon.com/KIT_PSE84_AI) (`KIT_PSE84_AI`) -
[Purchase Link](https://www.newark.com/infineon/kitpse84aitobo1/ai-eval-kit-32bit-arm-cortex-m55f/dp/49AM4459)

## Set Up The Project

To set up the project, please refer to the 
[/IOTCONNECT ModusToolbox&trade; PSOC Edge Developer Guide](DEVELOPER_GUIDE.md)

To quickly evaluate the project without development tools, you can download the pre-built Gestures binary package at
[avnet-iotc-mtb-psoc-edge-aivision-webrtc-v1.0.0.hex.zip](https://downloads.iotconnect.io/partners/infineon/demos/avnet-iotc-mtb-psoc-edge-aivision-webrtc-v1.0.0.hex.zip).
You can skip the VSCode and compiler setup in the Developer Guide and flash the extracted hex file with the MTB Programmer software.
When flashing, ensure to select the "External Memory" option.

To trigger the certificate to be re-generated, click the Programmer's *Erase* button when connected to the board and program the firmware again.

## Running The Demo

- After a few seconds, the device will connect to /IOTCONNECT, and begin sending telemetry packets similar to the example below:
```
>>: {"d":[{"d":{"version":"1.0.0","random":48,"class_id":0,"class":"unlabelled","event_detected":false}}]}
```

- In your /IOTCONNECT Web UI, navigate to **Video Streaming** and click the **Start**. Allow some time for the stream to start. 

- You should be able to see the video stream and the inference highlighting the detected gesture as a colored square.
Placing your hand in the DVP camera's field of view will trigger the inference:

| Gesture  | Class ID | Color  | Description               |
|:---------|----------|--------|:--------------------------|
| Scissors | 1        | Green  | Two fingers extended      |
| Paper    | 2        | Orange | Open hand, fingers spread |
| Rock     | 3        | Blue   | Clenched fist             |

- The following commands can be sent to the device using the /IOTCONNECT Web UI:

| Command                  | Argument Type     | Description                                                                                             |
|:-------------------------|-------------------|:--------------------------------------------------------------------------------------------------------|
| `board-user-led`         | String (on/off)   | Turn the board LED on or off (Red LED on the EVK, Green on the AI)                                      |
| `set-reporting-interval` | Number (eg. 2000) | Set telemetry reporting interval in milliseconds.  By default, the application will report every 2000ms |


## Known Issues and Limitations

- H.264 encoding performance degradation is observed with the LLVM toolchain compared to GCC.
- Display corruption issues affecting both the LCD and WebRTC streams have been observed.
We suspect that this issue has to do with UART and GPU contention while CM33 is active.
  - Severe LCD corruption during the Device Configuration setup or while the *Do you wish to configure the board* prompt is displayed on the screen. 
  - Occasional horizontal colored lines appearing on LCD and WebRTC stream.
  - Due to this, the H.264 encoding has been artificially capped at around 3FPS, while the encoding + LCD + inference performance is 5+ FPFS and close to the 6.25 FPS rate of the DVP camera stream.
- The project does not yet implement the ability to refresh AWS credentials. WebRTC will work for about an hour before the board will need to be reset to start with new AWS credentials. 
