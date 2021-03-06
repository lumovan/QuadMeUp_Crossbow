/*
This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this file,
You can obtain one at http://mozilla.org/MPL/2.0/.

Copyright (c) 20xx, MPL Contributor1 contrib1@example.net
*/

#include "config.h"

#include "lora.h"
#include "radio_node.h"
#include "variables.h"
#include "main_variables.h"
#include "qsp.h"
#include "sbus.h"
#include "platform_node.h"

#ifdef ARDUINO_AVR_FEATHER32U4
    #define LORA_SS_PIN     8
    #define LORA_RST_PIN    4
    #define LORA_DI0_PIN    7

    #define BUTTON_0_PIN    9
    #define BUTTON_1_PIN    10
#elif defined(ARDUINO_SAMD_FEATHER_M0)
    #define LORA_SS_PIN     8
    #define LORA_RST_PIN    4
    #define LORA_DI0_PIN    3

    #define BUTTON_0_PIN    9 //Please verify
    #define BUTTON_1_PIN    10 //Please verify
#else
    #error please select hardware
#endif

RadioNode radioNode;
PlatformNode platformNode;

/*
 * Main defines for device working in TX mode
 */
#ifdef DEVICE_MODE_TX

#ifdef FEATURE_TX_INPUT_PPM
  #include "ppm_reader.h"
  PPM_Reader txInput(PPM_INPUT_PIN, true);

#elif defined(FEATURE_TX_INPUT_SBUS)
  #include "sbus.h"
  SbusInput txInput(Serial1);

#else
  #error please select tx input source
#endif

#include "txbuzzer.h"

BuzzerState_t buzzer;

#ifdef FEATURE_TX_OLED
#include "tx_oled.h"
TxOled oled;
#endif

#include "tactile.h"

Tactile button0(BUTTON_0_PIN);
Tactile button1(BUTTON_1_PIN);

#endif

/*
 * Main defines for device working in RX mode
 */
#ifdef DEVICE_MODE_RX
    uint32_t sbusTime = 0;
    uint8_t sbusPacket[SBUS_PACKET_LENGTH] = {0};
    uint32_t lastRxStateTaskTime = 0;
#endif

/*
 * Start of QSP protocol implementation
 */
QspConfiguration_t qsp = {};
RxDeviceState_t rxDeviceState = {};
TxDeviceState_t txDeviceState = {};

void onQspSuccess(QspConfiguration_t *qsp, TxDeviceState_t *txDeviceState, RxDeviceState_t *rxDeviceState, uint8_t receivedChannel) {
    //If recide received a valid frame, that means it can start to talk
    radioNode.lastReceivedChannel = receivedChannel;
    
    //RX can start transmitting only when an least one frame has been receiveds
    radioNode.canTransmit = true;

    radioNode.readRssi();
    radioNode.readSnr();

    /*
     * RX module hops to next channel after frame has been received
     */
#ifdef DEVICE_MODE_RX
    if (!platformNode.isBindMode) {
        //We do not hop frequency in bind mode!
        radioNode.hopFrequency(true, radioNode.lastReceivedChannel, millis());
        radioNode.failedDwellsCount = 0; // We received a frame, so we can just reset this counter
        LoRa.receive(); //Put radio back into receive mode
    }
#endif

    //Store the last timestamp when frame was received
    if (qsp->frameId < QSP_FRAME_COUNT) {
        qsp->lastFrameReceivedAt[qsp->frameId] = millis();
    }
    qsp->anyFrameRecivedAt = millis();
    switch (qsp->frameId) {
        case QSP_FRAME_RC_DATA:
            qspDecodeRcDataFrame(qsp, rxDeviceState);
            break;

        case QSP_FRAME_RX_HEALTH:
            decodeRxHealthPayload(qsp, rxDeviceState);
            break;

        case QSP_FRAME_PING:
            qsp->forcePongFrame = true;
            break;

        case QSP_FRAME_BIND:
#ifdef DEVICE_MODE_RX
            if (platformNode.isBindMode) {
                platformNode.bindKey[0] = qsp->payload[0];
                platformNode.bindKey[1] = qsp->payload[1];
                platformNode.bindKey[2] = qsp->payload[2];
                platformNode.bindKey[3] = qsp->payload[3];

                platformNode.saveBindKey(platformNode.bindKey);
                platformNode.leaveBindMode();
            }
#endif
            break;

        case QSP_FRAME_PONG:
            txDeviceState->roundtrip = qsp->payload[0];
            txDeviceState->roundtrip += (uint32_t) qsp->payload[1] << 8;
            txDeviceState->roundtrip += (uint32_t) qsp->payload[2] << 16;
            txDeviceState->roundtrip += (uint32_t) qsp->payload[3] << 24;

            txDeviceState->roundtrip = (micros() - txDeviceState->roundtrip) / 1000;
            break;

        default:
            //Unknown frame
            //TODO do something in this case
            break;
    }

    qsp->transmitWindowOpen = true;
}

void onQspFailure(QspConfiguration_t *qsp, TxDeviceState_t *txDeviceState, RxDeviceState_t *rxDeviceState) {

}

//I do not get function pointers to object methods, no way...
int getRcChannel_wrapper(uint8_t channel) {
    return platformNode.getRcChannel(channel);
}

//Same here, wrapper just works
void setRcChannel_wrapper(uint8_t channel, int value, int offset) {
    platformNode.setRcChannel(channel, value, offset);
}

void setup(void)
{
#ifdef DEBUG_SERIAL
    Serial.begin(115200);
#endif

    qsp.onSuccessCallback = onQspSuccess;
    qsp.onFailureCallback = onQspFailure;
    qsp.rcChannelGetCallback = getRcChannel_wrapper;
    qsp.setRcChannelCallback = setRcChannel_wrapper;

#ifdef DEVICE_MODE_RX
    platformNode.platformState = DEVICE_STATE_FAILSAFE;
#else
    platformNode.platformState = DEVICE_STATE_OK;

    txInput.setRcChannelCallback = setRcChannel_wrapper;

#endif

    radioNode.init(LORA_SS_PIN, LORA_RST_PIN, LORA_DI0_PIN, onReceive);

#ifdef DEVICE_MODE_RX

    pinMode(RX_ADC_PIN_1, INPUT);
    pinMode(RX_ADC_PIN_2, INPUT);
    pinMode(RX_ADC_PIN_3, INPUT);

    /*
     * Prepare Serial1 for S.Bus processing
     */
    Serial1.begin(100000, SERIAL_8E2);

    platformNode.enterBindMode();
    LoRa.receive(); //TODO this probably should be moved somewhere....
#endif

#ifdef DEVICE_MODE_TX

    randomSeed(analogRead(A4));
    platformNode.seed();

#ifdef FEATURE_TX_OLED
    oled.init();
    oled.page(TX_PAGE_INIT);
#endif

    /*
     * TX should start talking imediately after power up
     */
    radioNode.canTransmit = true;

    pinMode(TX_BUZZER_PIN, OUTPUT);

    //Play single tune to indicate power up
    buzzerSingleMode(BUZZER_MODE_CHIRP, &buzzer);

    /*
     * Prepare Serial1 for S.Bus processing
     */
    txInput.start();

    /*
     * Buttons on TX module
     */
    button0.start();
    button1.start();

    platformNode.loadBindKey(platformNode.bindKey);

#endif

    pinMode(LED_BUILTIN, OUTPUT);
}

uint8_t currentSequenceIndex = 0;
#define TRANSMIT_SEQUENCE_COUNT 16

#ifdef DEVICE_MODE_RX

void updateRxDeviceState(RxDeviceState_t *rxDeviceState) {
    rxDeviceState->rxVoltage = map(analogRead(RX_ADC_PIN_1), 0, 1024, 0, 255);
    rxDeviceState->a1Voltage = map(analogRead(RX_ADC_PIN_2), 0, 1024, 0, 255);
    rxDeviceState->a2Voltage = map(analogRead(RX_ADC_PIN_3), 0, 1024, 0, 255);
}

int8_t getFrameToTransmit(QspConfiguration_t *qsp) {

    if (qsp->forcePongFrame) {
        qsp->forcePongFrame = false;
        return QSP_FRAME_PONG;
    }

    int8_t retVal = rxSendSequence[currentSequenceIndex];

    currentSequenceIndex++;
    if (currentSequenceIndex >= TRANSMIT_SEQUENCE_COUNT) {
        currentSequenceIndex = 0;
    }

    return retVal;
}

#endif

#ifdef DEVICE_MODE_TX
int8_t getFrameToTransmit(QspConfiguration_t *qsp) {

    if (platformNode.isBindMode) {
        return QSP_FRAME_BIND;
    }

    int8_t retVal = txSendSequence[currentSequenceIndex];

    currentSequenceIndex++;
    if (currentSequenceIndex >= TRANSMIT_SEQUENCE_COUNT) {
        currentSequenceIndex = 0;
    }

    return retVal;
}
#endif

/*
 *
 * Main loop starts here!
 *
 */
void loop(void)
{

    static uint32_t nextKey = millis();

    uint32_t currentMillis = millis();

#ifdef DEVICE_MODE_RX

    //Make sure to leave bind mode when binding is done
    if (platformNode.isBindMode && millis() > platformNode.bindModeExitMillis) {
        platformNode.leaveBindMode();
    }

    /*
     * This routine handles resync of TX/RX while hoppping frequencies
     * When not in bind mode. Bind mode is single frequency operation
     */
    if (!platformNode.isBindMode) {
        radioNode.handleChannelDwell();
    }

    /*
     * Detect the moment when radio module stopped transmittig and put it
     * back in to receive state
     */
    radioNode.handleTxDoneState(false);
#else 

    //Process buttons
    button0.loop();
    button1.loop();

#ifdef FEATURE_TX_OLED
    oled.loop();
#endif

    txInput.recoverStuckFrames();

    /*
     * If we are not receiving SBUS frames from radio, try to restart serial
     */
    static uint32_t serialRestartMillis = 0;

    /*
     * Final guard for SBUS input. If there is no input, try to restart serial port
     */
    if (!txInput.isReceiving() && serialRestartMillis + 100 < currentMillis) {
        txInput.restart();
        serialRestartMillis = currentMillis;
    }

    radioNode.handleTxDoneState(!platformNode.isBindMode);
#endif

    radioNode.readAndDecode(
        &qsp,
        &rxDeviceState,
        &txDeviceState,
        platformNode.bindKey
    );

    bool transmitPayload = false;

    /*
     * Watchdog for frame decoding stuck somewhere in the middle of a process
     */
    if (
        qsp.protocolState != QSP_STATE_IDLE &&
        qsp.frameDecodingStartedAt + QSP_MAX_FRAME_DECODE_TIME < currentMillis
    ) {
        qsp.protocolState = QSP_STATE_IDLE;
    }

#ifdef DEVICE_MODE_TX

    txInput.loop();

    if (
        radioNode.radioState == RADIO_STATE_RX &&
        qsp.protocolState == QSP_STATE_IDLE &&
        qsp.lastTxSlotTimestamp + TX_TRANSMIT_SLOT_RATE < currentMillis
    ) {
        
        int8_t frameToSend = getFrameToTransmit(&qsp);

    #ifndef FORCE_TX_WITHOUT_INPUT
        /*
         * If module is not receiving data from radio, do not send RC DATA
         * This is the only way to trigger failsafe in that case
         */
        if (frameToSend == QSP_FRAME_RC_DATA && !txInput.isReceiving()) {
            frameToSend = -1;
        }
    #endif

        if (frameToSend > -1) {

            qsp.frameToSend = frameToSend;
            qspClearPayload(&qsp);

            switch (qsp.frameToSend) {
                case QSP_FRAME_PING:
                    encodePingPayload(&qsp, micros());
                    break;

                case QSP_FRAME_RC_DATA:
                    encodeRcDataPayload(&qsp, PLATFORM_CHANNEL_COUNT);
                    break;

                case QSP_FRAME_BIND:

                    /*
                     * Key to be transmitted is stored in EEPROM
                     * During binding different key is used
                     */ 
                    uint8_t key[4];
                    platformNode.loadBindKey(key);

                    encodeBindPayload(&qsp, key);
                    break;
            }

            transmitPayload = true;
        }

        qsp.lastTxSlotTimestamp = currentMillis;
    }

#endif

#ifdef DEVICE_MODE_RX
    /*
     * This routine updates RX device state and updates one of radio channels with RSSI value
     */
    if (lastRxStateTaskTime + RX_TASK_HEALTH < currentMillis) {
        lastRxStateTaskTime = currentMillis;
        updateRxDeviceState(&rxDeviceState);

        uint8_t output = constrain(radioNode.rssi - 40, 0, 100);

        rxDeviceState.indicatedRssi = (output * 10) + 1000;
    }

    /*
     * Main routine to answer to TX module
     */
    if (qsp.transmitWindowOpen && qsp.protocolState == QSP_STATE_IDLE) {
        qsp.transmitWindowOpen = false;

        int8_t frameToSend = getFrameToTransmit(&qsp);
        if (frameToSend > -1) {
            qsp.frameToSend = frameToSend;

            if (frameToSend != QSP_FRAME_PONG) {
                qspClearPayload(&qsp);
            }
            switch (qsp.frameToSend) {
                case QSP_FRAME_PONG:
                    /*
                     * Pong frame just responses with received payload
                     */
                    break;

                case QSP_FRAME_RX_HEALTH:
                    encodeRxHealthPayload(
                        &qsp, 
                        &rxDeviceState, 
                        radioNode.rssi, 
                        radioNode.snr, 
                        (platformNode.platformState == DEVICE_STATE_FAILSAFE)
                    );
                    break;
            }

            transmitPayload = true;
        }

    }

    if (currentMillis > sbusTime) {
        platformNode.setRcChannel(RSSI_CHANNEL - 1, rxDeviceState.indicatedRssi, 0);

        sbusPreparePacket(sbusPacket, false, (platformNode.platformState == DEVICE_STATE_FAILSAFE), getRcChannel_wrapper);
        Serial1.write(sbusPacket, SBUS_PACKET_LENGTH);
        sbusTime = currentMillis + SBUS_UPDATE_RATE;
    }

    if (qsp.lastFrameReceivedAt[QSP_FRAME_RC_DATA] + RX_FAILSAFE_DELAY < currentMillis) {
        platformNode.platformState = DEVICE_STATE_FAILSAFE;
        rxDeviceState.indicatedRssi = 0;
        radioNode.rssi = 0;
    } else {
        platformNode.platformState = DEVICE_STATE_OK;
    }

#endif

    if (transmitPayload)
    {
        radioNode.handleTx(&qsp, platformNode.bindKey);
    }

#ifdef DEVICE_MODE_TX

    buzzerProcess(TX_BUZZER_PIN, currentMillis, &buzzer);

    // This routing enables when TX starts to receive signal from RX for a first time or after
    // failsafe
    if (txDeviceState.isReceiving == false && qsp.anyFrameRecivedAt != 0) {
        //TX module started to receive data
        buzzerSingleMode(BUZZER_MODE_DOUBLE_CHIRP, &buzzer);
        txDeviceState.isReceiving = true;
        platformNode.platformState = DEVICE_STATE_OK;
    }

    //Here we detect failsafe state on TX module
    if (
        txDeviceState.isReceiving &&
        qsp.anyFrameRecivedAt + TX_FAILSAFE_DELAY < currentMillis
    ) {
        txDeviceState.isReceiving = false;
        rxDeviceState.a1Voltage = 0;
        rxDeviceState.a2Voltage = 0;
        rxDeviceState.rxVoltage = 0;
        rxDeviceState.rssi = 0;
        rxDeviceState.snr = 0;
        rxDeviceState.flags = 0;
        txDeviceState.roundtrip = 0;
        platformNode.platformState = DEVICE_STATE_FAILSAFE;
        qsp.anyFrameRecivedAt = 0;
    }

    //FIXME rxDeviceState should be resetted also in RC_HEALT frame is not received in a long period

    //Handle audible alarms
    if (platformNode.platformState == DEVICE_STATE_FAILSAFE) {
        //Failsafe detected by TX
        buzzerContinousMode(BUZZER_MODE_SLOW_BEEP, &buzzer);
    } else if (txDeviceState.isReceiving && (rxDeviceState.flags & 0x1) == 1) {
        //Failsafe reported by RX module
        buzzerContinousMode(BUZZER_MODE_SLOW_BEEP, &buzzer);
    } else if (txDeviceState.isReceiving && radioNode.rssi < 45) {
        buzzerContinousMode(BUZZER_MODE_DOUBLE_CHIRP, &buzzer); // RSSI below 45dB // Critical state
    } else if (txDeviceState.isReceiving && radioNode.rssi < 55) {
        buzzerContinousMode(BUZZER_MODE_CHIRP, &buzzer); // RSSI below 55dB // Warning state
    } else {
        buzzerContinousMode(BUZZER_MODE_OFF, &buzzer);
    }

#endif

    /*
     * Handle LED updates
     */
    if (platformNode.nextLedUpdate < currentMillis) {
#ifdef DEVICE_MODE_TX
        if (txDeviceState.isReceiving) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            platformNode.nextLedUpdate = currentMillis + 300;
        } else if (txInput.isReceiving()) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            platformNode.nextLedUpdate = currentMillis + 100;
        } else {
            digitalWrite(LED_BUILTIN, HIGH);
            platformNode.nextLedUpdate = currentMillis + 200;
        }
#else

        if (platformNode.isBindMode) {
            platformNode.nextLedUpdate = currentMillis + 50;
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        } else {
            platformNode.nextLedUpdate = currentMillis + 200;

            if (platformNode.platformState == DEVICE_STATE_FAILSAFE) {
                digitalWrite(LED_BUILTIN, HIGH);
            } else {
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            }
        }
#endif
    }

}

void onReceive(int packetSize)
{
    /*
     * We can start reading only when radio is not reading.
     * If not reading, then we might start
     */
    if (radioNode.bytesToRead == NO_DATA_TO_READ) {
        if (packetSize >= MIN_PACKET_SIZE && packetSize <= MAX_PACKET_SIZE) {
            //We have a packet candidate that might contain a valid QSP packet
            radioNode.bytesToRead = packetSize;
        } else {
            /*
            That packet was not very interesting, just flush it, we have no use
            */
            LoRa.sleep();
            LoRa.receive();
            radioNode.radioState = RADIO_STATE_RX;
        }
    }
}