## Avnet PSOC™ Edge DEEPCRAFT™ Ready Models

This demo project is the integration of Infineon's 
[PSOC&trade; Edge MCU: DEEPCRAFT&trade; Ready Model deployment](https://github.com/Infineon/mtb-example-psoc-edge-ml-deepcraft-deploy-ready-model/tree/release-v1.2.0)
and [Avnet /IOTCONNECT ModusToolbox&trade; SDK](https://github.com/avnet-iotconnect/avnet-iotc-mtb-sdk). 

The project includes various DEEPCRAFT&trade; Ready Model which are chosen at compile time by 
selecting the model in [common.mk](common.mk)

The audio models detect specific sounds:
- Baby cry
- Cough
- Alarm

Additional models perform different ML model detections with various board sensors:
- Radar Gestures Model - Recognizes hand gestures in front of the board.
- Direction of Arrival (Audio) - Not supported at the moment.
- Fall Detection - Uses accelerometer data from the BMI270 sensor to detect a person falling, while the board is attached to the person's wrist.

Pre-trained models that are ready for production, referred to as "Ready Models," can be found on the [Imagimob Ready Model Landing Page](https://www.imagimob.com/ready-models). These models, when deployed on a device, are intended specifically for testing purposes and come with a limited number of inferences.

This project has a three project structure: CM33 secure, CM33 non-secure, and CM55 projects. All three projects are programmed to the external QSPI flash and executed in Execute in Place (XIP) mode. Extended boot launches the CM33 secure project from a fixed location in the external flash, which then configures the protection settings and launches the CM33 non-secure application. Additionally, CM33 non-secure application enables CM55 CPU and launches the CM55 application.

The M55 processor performs the DEEPCRAFT™ model heavy lifting and reports the data via IPC to the M33 processor.
The M33 Non-Secure application is a custom /IOTCONNECT application that is receiving the IPC messages, 
processing the data and sending it to /IOTCONNECT. 
This application can receive Cloud-To-Device commands as well and control one of the board LEDs or control the application flow.    

## Requirements

- [ModusToolbox&trade;](https://www.infineon.com/modustoolbox) with MTB Tools v3.6 or later (tested with v3.6)
- Board support package (BSP) minimum required version: 1.0.0
- Programming language: C
- Associated parts: All [PSOC&trade; Edge MCU](https://www.infineon.com/products/microcontroller/32-bit-psoc-arm-cortex/32-bit-psoc-edge-arm) parts

## Supported toolchains (make variable 'TOOLCHAIN')

- GNU Arm&reg; Embedded Compiler v14.2.1 (`GCC_ARM`) – Default value of `TOOLCHAIN`

> **Note:**
> This code example fails to build in RELEASE mode with the GCC_ARM toolchain v14.2.1 as it does not recognize some of the Helium instructions of the CMSIS-DSP library.

## Supported kits (make variable 'TARGET')

- [PSOC&trade; Edge E84 AI Kit](https://www.infineon.com/KIT_PSE84_AI) (`KIT_PSE84_AI`) -
[Purchase Link](https://www.newark.com/infineon/kitpse84aitobo1/ai-eval-kit-32bit-arm-cortex-m55f/dp/49AM4459)
- [PSOC&trade; Edge E84 Evaluation Kit](https://www.infineon.com/KIT_PSE84_EVAL) (`KIT_PSE84_EVAL_EPC2`) -
[Purchase Link](https://www.newark.com/infineon/kitpse84evaltobo1/eval-kit-32bit-arm-cortex-m55f/dp/49AM4460)


## Set Up The Project

To set up the project, please refer to the 
[/IOTCONNECT ModusToolbox&trade; PSOC Edge Developer Guide](DEVELOPER_GUIDE.md)

To quickly evaluate the project without development tools, you can download the pre-built Gestures binary package at
[avnet-iotc-mtb-psoc-edge-rm-gestures-v2.0.0.hex.zip](https://downloads.iotconnect.io/partners/infineon/demos/avnet-iotc-mtb-psoc-edge-rm-gestures-v2.0.0.hex.zip).
You can skip the VSCode and compiler setup in the Developer Guide and flash the extracted hex file with the MTB Programmer software.
When flashing, ensure to select the "External Memory" option.

To trigger the certificate to be re-generated, click the Programmer's *Erase* button when connected to the board and program the firmware again.

- To select the model, update the `MODEL_SELECTION` variable in the [common.mk](common.mk):

| Model name                  | Macro                       |
|:----------------------------|:----------------------------|
| Cough detection             | `COUGH_MODEL`               |
| Alarm detection             | `ALARM_MODEL`               |
| Baby cry detection          | `BABYCRY_MODEL`             |
| Gesture detection           | `GESTURE_MODEL`             |
| Directio of Arrival (Sound) | `DIRECTIONOFARRIVAL_MODEL`  |
| Fall detection              | `FALLDETECTION_MODEL`       |

> **Note:** Currently, gesture detection model is supported only for the PSOC&trade; Edge AI kit.

## Running The Demo

- For audio sound recognition models, once the board connects to /IOTCONNECT, 
it will start processing microphone input and attempt to detect the corresponding sound. 
This can be tested by placing the board in such way so that the microphone close to the PC speaker.


- The following YouTube sound clips can be used for testing the audio models:
  * [Baby Cry](https://www.youtube.com/watch?v=Rwj1_eWltJQ&t=265s)
  * [Cough](https://www.youtube.com/watch?v=Qp09X74kjBc)
  * [Alarm](https://www.youtube.com/watch?v=hFIJaB6kVzk)


- For Gesture detection model, if having issues with detections, 
place the kit at a distance of approximately 60 cms away from you,
for the gestures to be detected correctly. 
See the original Infineon project GitHub page for more details on how to perform gestures:
    * Push
    * Swipe Up
    * Swipe Down
    * Swipe Left
    * Swipe Right

- After a few seconds, the device will connect to /IOTCONNECT, and begin sending telemetry packets similar to the example below 
depending on the application version and the model selected (first letter in the version prefix):
```
>: {"d":[{"d":{"version":"B-1.1.1","random":32,,"class_id":1,"class":"baby_cry","event_detected":true}}]}
```
- The following commands can be sent to the device using the /IOTCONNECT Web UI:

    | Command                  | Argument Type     | Description                                                                                             |
    |:-------------------------|-------------------|:--------------------------------------------------------------------------------------------------------|
    | `board-user-led`         | String (on/off)   | Turn the board LED on or off (Red LED on the EVK, Green on the AI)                                      |
    | `set-reporting-interval` | Number (eg. 2000) | Set telemetry reporting interval in milliseconds.  By default, the application will report every 2000ms |
