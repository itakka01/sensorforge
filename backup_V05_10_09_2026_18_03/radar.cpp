#include "radar.h"
#include "board_config.h"

#include <HardwareSerial.h>
#include <math.h>


// =============================================================
// UART / PROTOCOL CONSTANTS
// =============================================================

static HardwareSerial radarSerial(1);

static const uint32_t RADAR_BAUD =
    115200UL;

static const uint32_t RADAR_RESPONSE_TIMEOUT_MS =
    800UL;

static const uint8_t COMMAND_HEADER[4] = {
    0xFD, 0xFC, 0xFB, 0xFA
};

static const uint8_t COMMAND_TAIL[4] = {
    0x04, 0x03, 0x02, 0x01
};

static const size_t MAX_COMMAND_PAYLOAD =
    192;


// Commands from the official HLK-LD2410S protocol.
static const uint16_t CMD_ENABLE_CONFIG  = 0x00FF;
static const uint16_t CMD_END_CONFIG     = 0x00FE;
static const uint16_t CMD_WRITE_GENERAL  = 0x0070;
static const uint16_t CMD_READ_GENERAL   = 0x0071;
static const uint16_t CMD_WRITE_TRIGGER  = 0x0072;
static const uint16_t CMD_READ_TRIGGER   = 0x0073;
static const uint16_t CMD_WRITE_HOLD     = 0x0076;
static const uint16_t CMD_READ_HOLD      = 0x0077;
static const uint16_t CMD_SET_OUTPUT_MODE = 0x007A;


// General parameter identifiers.
static const uint16_t PARAM_MAX_GATE       = 0x0005;
static const uint16_t PARAM_MIN_GATE       = 0x000A;
static const uint16_t PARAM_ABSENCE_SEC    = 0x0006;
static const uint16_t PARAM_STATUS_RATE    = 0x0002;
static const uint16_t PARAM_DISTANCE_RATE  = 0x000C;
static const uint16_t PARAM_RESPONSE_SPEED = 0x000B;


// =============================================================
// LIVE REPORT / MOTION STATE
// =============================================================

static uint8_t compactState =
    0;

static uint8_t compactTargetState =
    0;

static uint16_t compactDistance =
    0;


// Standard report parser.
// Standard payload type 0x01 is 70 bytes:
//   type(1), target state(1), distance(2), reserved(2),
//   16 * uint32 gate-energy values.
static uint8_t standardParseState =
    0;

static uint8_t standardHeaderMatch =
    0;

static uint16_t standardPayloadLength =
    0;

static uint16_t standardPayloadPosition =
    0;

static uint8_t standardTailPosition =
    0;

static uint8_t standardPayload[96];


static uint8_t lastTargetState =
    0;

static uint16_t lastTargetDistanceCm =
    0;

static uint32_t lastTargetReportMs =
    0;


static bool commandInProgress =
    false;


// Runtime parameters used by the fast ESP-side motion detector.
static bool motionTrackingConfigured =
    false;

static RadarSettings motionSettings;

static uint32_t standardModeStartedMs =
    0;

static uint32_t lastStandardReportMs =
    0;


// Latest standard-report gate energies, converted to the same dB scale
// used by the LD2410S trigger thresholds.
static float latestGateEnergyDb[16] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f
};


// Motion event remains active for this long after the last gate that
// reaches the LD2410S trigger threshold.
static const uint32_t RADAR_MOTION_HOLD_MS =
    2000UL;


static uint32_t lastMotionEventMs =
    0;

static int lastMotionGate =
    -1;

static float lastMotionEnergyDb =
    0.0f;


// Temporary software guard used after a radar configuration write
// during an active recording. This does not change last gate/energy.
static uint32_t motionGuardUntilMs =
    0;


// =============================================================
// LITTLE-ENDIAN HELPERS
// =============================================================

static uint16_t readU16LE(
    const uint8_t *p
)
{
    return
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8);
}


static uint32_t readU32LE(
    const uint8_t *p
)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}


static void appendU16LE(
    uint8_t *buffer,
    size_t &position,
    uint16_t value
)
{
    buffer[position++] =
        (uint8_t)(
            value & 0xFFU
        );

    buffer[position++] =
        (uint8_t)(
            (value >> 8) & 0xFFU
        );
}


static void appendU32LE(
    uint8_t *buffer,
    size_t &position,
    uint32_t value
)
{
    buffer[position++] =
        (uint8_t)(
            value & 0xFFUL
        );

    buffer[position++] =
        (uint8_t)(
            (value >> 8) & 0xFFUL
        );

    buffer[position++] =
        (uint8_t)(
            (value >> 16) & 0xFFUL
        );

    buffer[position++] =
        (uint8_t)(
            (value >> 24) & 0xFFUL
        );
}


// =============================================================
// UART HELPERS
// =============================================================

static void resetCompactParser()
{
    compactState =
        0;
}


static void resetStandardParser()
{
    standardParseState =
        0;

    standardHeaderMatch =
        0;

    standardPayloadLength =
        0;

    standardPayloadPosition =
        0;

    standardTailPosition =
        0;
}


static void resetReportParsers()
{
    resetCompactParser();
    resetStandardParser();
}


static void drainInput()
{
    while (radarSerial.available()) {
        radarSerial.read();
    }

    resetReportParsers();
}


static bool readByteUntil(
    uint8_t &value,
    uint32_t deadline
)
{
    while (
        (int32_t)(
            deadline -
            millis()
        ) > 0
    ) {

        if (radarSerial.available()) {

            int incoming =
                radarSerial.read();

            if (incoming >= 0) {
                value =
                    (uint8_t)incoming;

                return true;
            }
        }

        delay(1);
    }

    return false;
}


// Find one command-response frame.
// Normal compact reports may be interleaved before the ACK; they are skipped.
static bool readCommandFrame(
    uint8_t *payload,
    size_t payloadCapacity,
    size_t &payloadLength,
    uint32_t deadline
)
{
    payloadLength =
        0;

    uint8_t headerMatch =
        0;


    while (
        (int32_t)(
            deadline -
            millis()
        ) > 0
    ) {

        uint8_t value;

        if (!readByteUntil(
                value,
                deadline
            )) {

            return false;
        }


        if (
            value ==
            COMMAND_HEADER[
                headerMatch
            ]
        ) {

            headerMatch++;

            if (headerMatch < 4) {
                continue;
            }

        } else {

            headerMatch =
                (
                    value ==
                    COMMAND_HEADER[0]
                )
                ? 1
                : 0;

            continue;
        }


        uint8_t lengthBytes[2];

        if (
            !readByteUntil(
                lengthBytes[0],
                deadline
            ) ||
            !readByteUntil(
                lengthBytes[1],
                deadline
            )
        ) {

            return false;
        }


        uint16_t frameLength =
            readU16LE(
                lengthBytes
            );


        if (
            frameLength < 2 ||
            frameLength >
                MAX_COMMAND_PAYLOAD ||
            frameLength >
                payloadCapacity
        ) {

            headerMatch =
                0;

            continue;
        }


        for (
            uint16_t i = 0;
            i < frameLength;
            ++i
        ) {

            if (!readByteUntil(
                    payload[i],
                    deadline
                )) {

                return false;
            }
        }


        bool tailOk =
            true;

        for (
            uint8_t i = 0;
            i < 4;
            ++i
        ) {

            uint8_t tailByte;

            if (!readByteUntil(
                    tailByte,
                    deadline
                )) {

                return false;
            }


            if (
                tailByte !=
                COMMAND_TAIL[i]
            ) {

                tailOk =
                    false;
            }
        }


        if (!tailOk) {

            headerMatch =
                0;

            continue;
        }


        payloadLength =
            frameLength;

        return true;
    }


    return false;
}


static bool sendCommand(
    uint16_t command,
    const uint8_t *parameters,
    size_t parameterLength
)
{
    if (
        parameterLength >
        (MAX_COMMAND_PAYLOAD - 2)
    ) {

        return false;
    }


    uint16_t payloadLength =
        (uint16_t)(
            parameterLength +
            2U
        );


    radarSerial.write(
        COMMAND_HEADER,
        sizeof(COMMAND_HEADER)
    );


    uint8_t lengthBytes[2] = {
        (uint8_t)(
            payloadLength & 0xFFU
        ),
        (uint8_t)(
            (payloadLength >> 8) &
            0xFFU
        )
    };


    radarSerial.write(
        lengthBytes,
        sizeof(lengthBytes)
    );


    uint8_t commandBytes[2] = {
        (uint8_t)(
            command & 0xFFU
        ),
        (uint8_t)(
            (command >> 8) &
            0xFFU
        )
    };


    radarSerial.write(
        commandBytes,
        sizeof(commandBytes)
    );


    if (
        parameters &&
        parameterLength
    ) {

        radarSerial.write(
            parameters,
            parameterLength
        );
    }


    radarSerial.write(
        COMMAND_TAIL,
        sizeof(COMMAND_TAIL)
    );

    // IMPORTANT for Arduino-ESP32:
    // flush() without an argument also clears the RX buffer.
    // The LD2410S can answer immediately, so that would discard
    // its ACK before transact() gets a chance to read it.
    // txOnly=true waits until transmission is finished but keeps RX.
    radarSerial.flush(true);

    return true;
}


static bool transact(
    uint16_t command,
    const uint8_t *parameters,
    size_t parameterLength,
    uint8_t *responseData,
    size_t responseCapacity,
    size_t &responseLength,
    String &error
)
{
    responseLength =
        0;


    if (!sendCommand(
            command,
            parameters,
            parameterLength
        )) {

        error =
            "UART command too large";

        return false;
    }


    uint32_t deadline =
        millis() +
        RADAR_RESPONSE_TIMEOUT_MS;


    while (
        (int32_t)(
            deadline -
            millis()
        ) > 0
    ) {

        uint8_t frame[
            MAX_COMMAND_PAYLOAD
        ];

        size_t frameLength =
            0;


        if (!readCommandFrame(
                frame,
                sizeof(frame),
                frameLength,
                deadline
            )) {

            break;
        }


        if (frameLength < 2) {
            continue;
        }


        uint16_t responseCommand =
            readU16LE(
                frame
            );

        uint16_t expectedResponse =
            command |
            0x0100U;


        if (
            responseCommand !=
            expectedResponse
        ) {

            // Ignore unrelated command frames.
            continue;
        }


        size_t dataLength =
            frameLength -
            2U;


        if (
            dataLength >
            responseCapacity
        ) {

            error =
                "UART response too large";

            return false;
        }


        if (
            responseData &&
            dataLength
        ) {

            memcpy(
                responseData,
                frame + 2,
                dataLength
            );
        }


        responseLength =
            dataLength;

        return true;
    }


    error =
        "LD2410S UART timeout";

    return false;
}


// =============================================================
// CONFIG MODE
// =============================================================

static bool enterConfigMode(
    String &error
)
{
    drainInput();

    uint8_t parameter[2] = {
        0x01,
        0x00
    };

    uint8_t response[16];
    size_t responseLength =
        0;


    if (!transact(
            CMD_ENABLE_CONFIG,
            parameter,
            sizeof(parameter),
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    if (
        responseLength < 2 ||
        readU16LE(response) != 0
    ) {

        error =
            "LD2410S rejected config mode";

        return false;
    }


    return true;
}


static bool leaveConfigMode(
    String &error
)
{
    uint8_t response[8];
    size_t responseLength =
        0;


    if (!transact(
            CMD_END_CONFIG,
            nullptr,
            0,
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    if (
        responseLength < 2 ||
        readU16LE(response) != 0
    ) {

        error =
            "LD2410S rejected config exit";

        return false;
    }


    return true;
}


// =============================================================
// GENERAL PARAMETERS
// =============================================================

static bool readGeneralSettings(
    RadarSettings &settings,
    String &error
)
{
    const uint16_t ids[6] = {
        PARAM_MAX_GATE,
        PARAM_MIN_GATE,
        PARAM_ABSENCE_SEC,
        PARAM_STATUS_RATE,
        PARAM_DISTANCE_RATE,
        PARAM_RESPONSE_SPEED
    };

    uint8_t parameters[
        sizeof(ids)
    ];

    size_t position =
        0;


    for (
        uint8_t i = 0;
        i < 6;
        ++i
    ) {

        appendU16LE(
            parameters,
            position,
            ids[i]
        );
    }


    uint8_t response[64];
    size_t responseLength =
        0;


    if (!transact(
            CMD_READ_GENERAL,
            parameters,
            position,
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    // ACK + six uint32 values.
    if (
        responseLength <
        2U +
        6U * 4U
    ) {

        error =
            "LD2410S general response incomplete";

        return false;
    }


    if (readU16LE(response) != 0) {

        error =
            "LD2410S general read failed";

        return false;
    }


    const uint8_t *values =
        response +
        2;


    settings.maxGate =
        readU32LE(
            values + 0
        );

    settings.minGate =
        readU32LE(
            values + 4
        );

    settings.absenceSec =
        readU32LE(
            values + 8
        );

    settings.statusRateX10 =
        readU32LE(
            values + 12
        );

    settings.distanceRateX10 =
        readU32LE(
            values + 16
        );

    settings.responseSpeed =
        readU32LE(
            values + 20
        );


    return true;
}


static bool writeGeneralSettings(
    const RadarSettings &settings,
    String &error
)
{
    const uint16_t ids[6] = {
        PARAM_MAX_GATE,
        PARAM_MIN_GATE,
        PARAM_ABSENCE_SEC,
        PARAM_STATUS_RATE,
        PARAM_DISTANCE_RATE,
        PARAM_RESPONSE_SPEED
    };

    const uint32_t values[6] = {
        settings.maxGate,
        settings.minGate,
        settings.absenceSec,
        settings.statusRateX10,
        settings.distanceRateX10,
        settings.responseSpeed
    };


    uint8_t parameters[
        6U * 6U
    ];

    size_t position =
        0;


    for (
        uint8_t i = 0;
        i < 6;
        ++i
    ) {

        appendU16LE(
            parameters,
            position,
            ids[i]
        );

        appendU32LE(
            parameters,
            position,
            values[i]
        );
    }


    uint8_t response[8];
    size_t responseLength =
        0;


    if (!transact(
            CMD_WRITE_GENERAL,
            parameters,
            position,
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    if (
        responseLength < 2 ||
        readU16LE(response) != 0
    ) {

        error =
            "LD2410S general write failed";

        return false;
    }


    return true;
}


// =============================================================
// THRESHOLDS
// =============================================================

static bool readThresholdSet(
    uint16_t command,
    uint32_t values[16],
    String &error
)
{
    uint8_t parameters[
        16U * 2U
    ];

    size_t position =
        0;


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        appendU16LE(
            parameters,
            position,
            gate
        );
    }


    uint8_t response[
        2U +
        16U * 4U
    ];

    size_t responseLength =
        0;


    if (!transact(
            command,
            parameters,
            position,
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    if (
        responseLength <
        2U +
        16U * 4U
    ) {

        error =
            "LD2410S threshold response incomplete";

        return false;
    }


    if (readU16LE(response) != 0) {

        error =
            "LD2410S threshold read failed";

        return false;
    }


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        values[gate] =
            readU32LE(
                response +
                2U +
                gate * 4U
            );
    }


    return true;
}


static bool writeThresholdSet(
    uint16_t command,
    const uint32_t values[16],
    String &error
)
{
    uint8_t parameters[
        16U * 6U
    ];

    size_t position =
        0;


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        appendU16LE(
            parameters,
            position,
            gate
        );

        appendU32LE(
            parameters,
            position,
            values[gate]
        );
    }


    uint8_t response[8];
    size_t responseLength =
        0;


    if (!transact(
            command,
            parameters,
            position,
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    if (
        responseLength < 2 ||
        readU16LE(response) != 0
    ) {

        error =
            "LD2410S threshold write failed";

        return false;
    }


    return true;
}


// =============================================================
// STANDARD REPORT MODE / FAST MOTION
// =============================================================

static void cacheMotionSettings(
    const RadarSettings &settings
)
{
    motionSettings =
        settings;
}


static bool setStandardOutputMode(
    String &error
)
{
    // Official command 0x007A:
    // parameter-id 0x0000 + uint32 value 1 = standard data output.
    const uint8_t parameters[6] = {
        0x00, 0x00,
        0x01, 0x00, 0x00, 0x00
    };

    uint8_t response[8];
    size_t responseLength =
        0;


    if (!transact(
            CMD_SET_OUTPUT_MODE,
            parameters,
            sizeof(parameters),
            response,
            sizeof(response),
            responseLength,
            error
        )) {

        return false;
    }


    if (
        responseLength < 2 ||
        readU16LE(response) != 0
    ) {

        error =
            "LD2410S standard-output command failed";

        return false;
    }


    return true;
}


static float gateEnergyDb(
    uint32_t rawEnergy
)
{
    if (rawEnergy == 0)
        return 0.0f;

    // Hi-Link's documented conversion:
    // displayed threshold/energy = 10 * log10(raw UART energy).
    return
        10.0f *
        log10f(
            (float)rawEnergy
        );
}


static void processStandardPayload(
    const uint8_t *payload,
    uint16_t length
)
{
    // Standard target report:
    // 1 type + 1 state + 2 distance + 2 reserved + 64 gate-energy.
    if (
        length < 70 ||
        payload[0] != 0x01
    ) {

        return;
    }


    uint32_t now =
        millis();


    lastTargetState =
        payload[1];

    lastTargetDistanceCm =
        readU16LE(
            payload + 2
        );

    lastTargetReportMs =
        now;

    lastStandardReportMs =
        now;


    // Always cache all 16 gate energies for the live WebConfig view,
    // regardless of the currently configured min/max distance.
    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        uint32_t rawEnergy =
            readU32LE(
                payload +
                6U +
                (uint16_t)gate * 4U
            );

        latestGateEnergyDb[gate] =
            gateEnergyDb(
                rawEnergy
            );
    }


    if (!motionTrackingConfigured)
        return;


    // The general "minimum distance gate" is a distance boundary:
    // minGate=3 means approx. 2.1 m. Gate 4 is therefore the first
    // complete 0.7-m gate beyond that boundary.
    uint32_t firstGateValue =
        motionSettings.minGate == 0
        ? 0UL
        : motionSettings.minGate + 1UL;

    if (firstGateValue > 15UL) {
        firstGateValue = 15UL;
    }

    uint8_t firstGate =
        (uint8_t)firstGateValue;

    uint32_t finalGateValue =
        motionSettings.maxGate;

    if (finalGateValue > 15UL) {
        finalGateValue = 15UL;
    }

    uint8_t finalGate =
        (uint8_t)finalGateValue;


    if (firstGate > finalGate)
        return;


    bool motionNow =
        false;

    int strongestGate =
        -1;

    float strongestMarginDb =
        -1000.0f;

    float strongestEnergyDb =
        0.0f;


    for (
        uint8_t gate = firstGate;
        gate <= finalGate;
        ++gate
    ) {

        float energyDb =
            latestGateEnergyDb[
                gate
            ];

        float thresholdDb =
            (float)
            motionSettings.triggerThreshold[
                gate
            ];

        float marginDb =
            energyDb -
            thresholdDb;


        if (marginDb >= 0.0f) {

            motionNow =
                true;


            if (
                marginDb >
                strongestMarginDb
            ) {

                strongestMarginDb =
                    marginDb;

                strongestGate =
                    gate;

                strongestEnergyDb =
                    energyDb;
            }
        }
    }


    if (motionNow) {

        lastMotionEventMs =
            now;

        lastMotionGate =
            strongestGate;

        lastMotionEnergyDb =
            strongestEnergyDb;
    }
}


static void parseCompactByte(
    uint8_t value
)
{
    switch (compactState) {

        case 0:
            if (value == 0x6E) {
                compactState = 1;
            }
            break;

        case 1:
            compactTargetState =
                value;

            compactState =
                2;
            break;

        case 2:
            compactDistance =
                value;

            compactState =
                3;
            break;

        case 3:
            compactDistance |=
                (uint16_t)value << 8;

            compactState =
                4;
            break;

        case 4:

            if (value == 0x62) {

                lastTargetState =
                    compactTargetState;

                lastTargetDistanceCm =
                    compactDistance;

                lastTargetReportMs =
                    millis();
            }

            compactState =
                (value == 0x6E)
                ? 1
                : 0;

            break;

        default:
            compactState =
                0;
            break;
    }
}


static void parseStandardByte(
    uint8_t value
)
{
    static const uint8_t header[4] = {
        0xF4, 0xF3, 0xF2, 0xF1
    };

    static const uint8_t tail[4] = {
        0xF8, 0xF7, 0xF6, 0xF5
    };


    switch (standardParseState) {

        // Search frame header.
        case 0:

            if (
                value ==
                header[
                    standardHeaderMatch
                ]
            ) {

                standardHeaderMatch++;

                if (
                    standardHeaderMatch ==
                    4
                ) {

                    standardHeaderMatch =
                        0;

                    standardParseState =
                        1;
                }

            } else {

                standardHeaderMatch =
                    value == header[0]
                    ? 1
                    : 0;
            }

            break;


        // Payload length LSB.
        case 1:

            standardPayloadLength =
                value;

            standardParseState =
                2;

            break;


        // Payload length MSB.
        case 2:

            standardPayloadLength |=
                (uint16_t)value << 8;


            if (
                standardPayloadLength == 0 ||
                standardPayloadLength >
                    sizeof(standardPayload)
            ) {

                resetStandardParser();

            } else {

                standardPayloadPosition =
                    0;

                standardParseState =
                    3;
            }

            break;


        // Payload.
        case 3:

            standardPayload[
                standardPayloadPosition++
            ] =
                value;


            if (
                standardPayloadPosition >=
                standardPayloadLength
            ) {

                standardTailPosition =
                    0;

                standardParseState =
                    4;
            }

            break;


        // Tail.
        case 4:

            if (
                value ==
                tail[
                    standardTailPosition
                ]
            ) {

                standardTailPosition++;


                if (
                    standardTailPosition ==
                    4
                ) {

                    processStandardPayload(
                        standardPayload,
                        standardPayloadLength
                    );

                    resetStandardParser();
                }

            } else {

                resetStandardParser();

                // Permit immediate re-sync if this byte is also F4.
                if (value == header[0]) {
                    standardHeaderMatch = 1;
                }
            }

            break;


        default:

            resetStandardParser();

            break;
    }
}


// =============================================================
// PUBLIC API
// =============================================================

void radarBegin()
{
    radarSerial.begin(
        RADAR_BAUD,
        SERIAL_8N1,
        RADAR_RX_PIN,
        RADAR_TX_PIN
    );

    resetReportParsers();


    Serial.printf(
        "LD2410S UART: RX=%d TX=%d @ 115200\n",
        RADAR_RX_PIN,
        RADAR_TX_PIN
    );
}


bool radarStartMotionTracking(
    String &error
)
{
    error =
        "";

    motionTrackingConfigured =
        false;

    lastMotionEventMs =
        0;

    lastMotionGate =
        -1;

    lastMotionEnergyDb =
        0.0f;

    lastStandardReportMs =
        0;


    commandInProgress =
        true;

    bool configEntered =
        false;

    bool standardModeSet =
        false;

    RadarSettings settings;

    memset(
        &settings,
        0,
        sizeof(settings)
    );


    bool ok =
        enterConfigMode(
            error
        );


    if (ok) {

        configEntered =
            true;

        ok =
            readGeneralSettings(
                settings,
                error
            );
    }


    if (ok) {

        ok =
            readThresholdSet(
                CMD_READ_TRIGGER,
                settings.triggerThreshold,
                error
            );
    }


    if (ok) {

        ok =
            setStandardOutputMode(
                error
            );

        standardModeSet =
            ok;
    }


    if (configEntered) {

        String exitError;

        bool exitOk =
            leaveConfigMode(
                exitError
            );


        if (
            ok &&
            !exitOk
        ) {

            // Same behavior as the verified parameter-write path:
            // some modules occasionally miss only the final exit ACK.
            Serial.println(
                "LD2410S warning: startup config-exit ACK timeout"
            );
        }
    }


    commandInProgress =
        false;

    drainInput();


    if (
        !ok ||
        !standardModeSet
    ) {

        return false;
    }


    cacheMotionSettings(
        settings
    );

    motionTrackingConfigured =
        true;

    standardModeStartedMs =
        millis();


    Serial.printf(
        "LD2410S fast motion: standard mode, gates %lu..%lu, hold=%lu ms\n",
        (unsigned long)settings.minGate,
        (unsigned long)settings.maxGate,
        (unsigned long)RADAR_MOTION_HOLD_MS
    );


    return true;
}


void radarLoop()
{
    if (commandInProgress)
        return;


    // One standard frame is ~80 bytes. 192 bytes allows more than two
    // complete reports per main-loop pass while keeping work bounded.
    uint16_t processed =
        0;


    while (
        radarSerial.available() &&
        processed < 192
    ) {

        int incoming =
            radarSerial.read();

        if (incoming < 0)
            break;


        uint8_t value =
            (uint8_t)incoming;

        processed++;


        if (motionTrackingConfigured) {

            parseStandardByte(
                value
            );

        } else {

            parseCompactByte(
                value
            );
        }
    }
}

void radarGetHiLinkDefaultSettings(
    RadarSettings &settings
)
{
    // Hi-Link LD2410S host-tool defaults (manual v1.04):
    // 0.0..8.4 m, 4.0 Hz status, 0.5 Hz distance, normal response,
    // 40 s absence delay, plus the per-gate trigger/hold thresholds below.
    static const uint32_t triggerDefaults[16] = {
        48, 42, 36, 34, 32, 31, 31, 31,
        31, 31, 31, 31, 31, 31, 31, 31
    };

    static const uint32_t holdDefaults[16] = {
        45, 42, 33, 32, 28, 28, 28, 28,
        28, 28, 28, 28, 28, 28, 28, 28
    };

    memset(
        &settings,
        0,
        sizeof(settings)
    );

    settings.minGate =
        0;

    settings.maxGate =
        12; // 12 * 0.7 m = 8.4 m

    settings.absenceSec =
        40;

    settings.statusRateX10 =
        40; // 4.0 Hz

    settings.distanceRateX10 =
        5;  // 0.5 Hz

    settings.responseSpeed =
        5;  // normal

    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {
        settings.triggerThreshold[gate] =
            triggerDefaults[gate];

        settings.holdThreshold[gate] =
            holdDefaults[gate];
    }
}


bool radarValidateSettings(
    const RadarSettings &settings,
    String &error
)
{
    if (
        settings.maxGate < 1 ||
        settings.maxGate > 16
    ) {

        error =
            "max gate must be 1..16";

        return false;
    }


    if (settings.minGate > 16) {

        error =
            "min gate must be 0..16";

        return false;
    }


    if (
        settings.minGate >
        settings.maxGate
    ) {

        error =
            "min gate must not exceed max gate";

        return false;
    }


    if (
        settings.absenceSec < 10 ||
        settings.absenceSec > 120
    ) {

        error =
            "absence time must be 10..120 s";

        return false;
    }


    if (
        settings.statusRateX10 < 5 ||
        settings.statusRateX10 > 80 ||
        settings.statusRateX10 % 5 != 0
    ) {

        error =
            "status report rate must be 0.5..8.0 Hz";

        return false;
    }


    if (
        settings.distanceRateX10 < 5 ||
        settings.distanceRateX10 > 80 ||
        settings.distanceRateX10 % 5 != 0
    ) {

        error =
            "distance report rate must be 0.5..8.0 Hz";

        return false;
    }


    if (
        settings.responseSpeed != 5 &&
        settings.responseSpeed != 10
    ) {

        error =
            "response speed must be normal or fast";

        return false;
    }


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        if (
            settings.triggerThreshold[gate] > 95
        ) {

            error =
                "trigger threshold gate " +
                String(gate) +
                " must be 0..95";

            return false;
        }


        if (
            settings.holdThreshold[gate] > 95
        ) {

            error =
                "hold threshold gate " +
                String(gate) +
                " must be 0..95";

            return false;
        }
    }


    return true;
}


bool radarReadSettings(
    RadarSettings &settings,
    String &error
)
{
    error =
        "";

    commandInProgress =
        true;

    bool configEntered =
        false;

    bool ok =
        enterConfigMode(
            error
        );


    if (ok) {
        configEntered =
            true;

        ok =
            readGeneralSettings(
                settings,
                error
            );
    }


    if (ok) {
        ok =
            readThresholdSet(
                CMD_READ_TRIGGER,
                settings.triggerThreshold,
                error
            );
    }


    if (ok) {
        ok =
            readThresholdSet(
                CMD_READ_HOLD,
                settings.holdThreshold,
                error
            );
    }


    if (configEntered) {

        String exitError;

        bool exitOk =
            leaveConfigMode(
                exitError
            );


        if (
            ok &&
            !exitOk
        ) {

            error =
                exitError;

            ok =
                false;
        }
    }


    commandInProgress =
        false;

    resetReportParsers();


    if (ok) {
        cacheMotionSettings(
            settings
        );
    }


    return ok;
}


static bool settingsEqual(
    const RadarSettings &a,
    const RadarSettings &b
)
{
    if (
        a.maxGate != b.maxGate ||
        a.minGate != b.minGate ||
        a.absenceSec != b.absenceSec ||
        a.statusRateX10 != b.statusRateX10 ||
        a.distanceRateX10 != b.distanceRateX10 ||
        a.responseSpeed != b.responseSpeed
    ) {

        return false;
    }


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        if (
            a.triggerThreshold[gate] !=
                b.triggerThreshold[gate] ||
            a.holdThreshold[gate] !=
                b.holdThreshold[gate]
        ) {

            return false;
        }
    }


    return true;
}


bool radarWriteSettings(
    const RadarSettings &settings,
    String &error
)
{
    if (!radarValidateSettings(
            settings,
            error
        )) {

        return false;
    }


    error =
        "";

    commandInProgress =
        true;

    bool configEntered =
        false;

    bool ok =
        enterConfigMode(
            error
        );


    RadarSettings verified;


    if (ok) {
        configEntered =
            true;

        ok =
            writeGeneralSettings(
                settings,
                error
            );
    }


    if (ok) {
        ok =
            writeThresholdSet(
                CMD_WRITE_TRIGGER,
                settings.triggerThreshold,
                error
            );
    }


    if (ok) {
        ok =
            writeThresholdSet(
                CMD_WRITE_HOLD,
                settings.holdThreshold,
                error
            );
    }


    // Verify while we are still in the SAME configuration session.
    // The official LD2410S protocol flow is:
    //   enable config -> write/read commands -> end config.
    //
    // Do not exit and immediately re-enter config mode just for the
    // verification read. Some module/firmware revisions need recovery
    // time after returning to normal operating mode.
    if (ok) {
        ok =
            readGeneralSettings(
                verified,
                error
            );

        if (!ok) {
            error =
                "write succeeded, verification general read failed: " +
                error;
        }
    }


    if (ok) {
        ok =
            readThresholdSet(
                CMD_READ_TRIGGER,
                verified.triggerThreshold,
                error
            );

        if (!ok) {
            error =
                "write succeeded, verification trigger read failed: " +
                error;
        }
    }


    if (ok) {
        ok =
            readThresholdSet(
                CMD_READ_HOLD,
                verified.holdThreshold,
                error
            );

        if (!ok) {
            error =
                "write succeeded, verification hold read failed: " +
                error;
        }
    }


    if (
        ok &&
        !settingsEqual(
            settings,
            verified
        )
    ) {

        error =
            "LD2410S verification mismatch";

        ok =
            false;
    }


    // Always try to return the radar to normal operating mode once
    // configuration mode was entered, even when write/verification failed.
    if (configEntered) {

        String exitError;

        bool exitOk =
            leaveConfigMode(
                exitError
            );


        if (
            ok &&
            !exitOk
        ) {

            // All write commands were ACKed and every parameter was
            // already read back and compared successfully.
            //
            // Some LD2410S modules occasionally do not deliver the ACK
            // for "end configuration" reliably even though they execute
            // the command. The module also auto-exits configuration mode
            // after a short command-idle period.
            //
            // Therefore this specific final ACK timeout is non-fatal.
            Serial.println(
                "LD2410S warning: config-exit ACK timeout; "
                "parameters were written and verified"
            );

            error =
                "";
        }
    }


    commandInProgress =
        false;

    resetReportParsers();


    if (ok) {
        cacheMotionSettings(
            verified
        );
    }


    return ok;
}

bool radarReportIsRecent(
    uint32_t maxAgeMs
)
{
    if (lastTargetReportMs == 0)
        return false;

    return
        (uint32_t)(
            millis() -
            lastTargetReportMs
        ) <=
        maxAgeMs;
}


uint8_t radarLastTargetState()
{
    return lastTargetState;
}


uint16_t radarLastTargetDistanceCm()
{
    return lastTargetDistanceCm;
}


bool radarMotionTrackingAvailable()
{
    if (!motionTrackingConfigured)
        return false;


    // During the first three seconds after switching modes, consider
    // tracking available even before the first complete standard frame.
    if (lastStandardReportMs == 0) {
        return
            (uint32_t)(
                millis() -
                standardModeStartedMs
            ) <=
            3000UL;
    }


    // At the intended 8 Hz this is extremely generous; it also remains
    // compatible with a 0.5-Hz status rate during testing.
    return
        (uint32_t)(
            millis() -
            lastStandardReportMs
        ) <=
        3000UL;
}


bool radarGetCachedSettings(
    RadarSettings &settings
)
{
    if (!motionTrackingConfigured)
        return false;

    settings =
        motionSettings;

    return true;
}


void radarHoldMotion(
    uint32_t durationMs
)
{
    if (
        !motionTrackingConfigured ||
        durationMs == 0
    ) {
        return;
    }

    uint32_t now =
        millis();

    uint32_t requestedUntil =
        now +
        durationMs;

    // Extend an existing guard, never shorten it.
    if (
        motionGuardUntilMs == 0 ||
        (int32_t)(
            requestedUntil -
            motionGuardUntilMs
        ) > 0
    ) {
        motionGuardUntilMs =
            requestedUntil;
    }
}


static uint32_t radarMotionGuardRemainingMs()
{
    if (motionGuardUntilMs == 0)
        return 0;

    uint32_t now =
        millis();

    if (
        (int32_t)(
            motionGuardUntilMs -
            now
        ) <= 0
    ) {
        motionGuardUntilMs =
            0;
        return 0;
    }

    return
        motionGuardUntilMs -
        now;
}


bool radarMotionActive()
{
    if (!motionTrackingConfigured)
        return false;

    if (
        radarMotionGuardRemainingMs() > 0
    ) {
        return true;
    }

    if (lastMotionEventMs == 0)
        return false;

    return
        (uint32_t)(
            millis() -
            lastMotionEventMs
        ) <=
        RADAR_MOTION_HOLD_MS;
}


uint32_t radarMotionRemainingMs()
{
    if (!motionTrackingConfigured)
        return 0;

    uint32_t remaining =
        radarMotionGuardRemainingMs();

    if (lastMotionEventMs != 0) {

        uint32_t elapsed =
            (uint32_t)(
                millis() -
                lastMotionEventMs
            );

        uint32_t eventRemaining =
            elapsed >= RADAR_MOTION_HOLD_MS
            ? 0
            : RADAR_MOTION_HOLD_MS -
                elapsed;

        if (eventRemaining > remaining) {
            remaining =
                eventRemaining;
        }
    }

    return remaining;
}


int radarLastMotionGate()
{
    return lastMotionGate;
}


float radarLastMotionEnergyDb()
{
    return lastMotionEnergyDb;
}


bool radarGateEnergyIsRecent(
    uint32_t maxAgeMs
)
{
    if (lastStandardReportMs == 0)
        return false;

    return
        (uint32_t)(
            millis() -
            lastStandardReportMs
        ) <=
        maxAgeMs;
}


float radarGateEnergyDb(
    uint8_t gate
)
{
    if (gate >= 16)
        return 0.0f;

    return
        latestGateEnergyDb[
            gate
        ];
}


int radarRxPin()
{
    return RADAR_RX_PIN;
}


int radarTxPin()
{
    return RADAR_TX_PIN;
}
