/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * telemetry_mavlink.c
 *
 * Author: Konstantin Sharlaimov
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#if defined(USE_TELEMETRY) && defined(USE_TELEMETRY_MAVLINK)

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/color.h"
#include "common/maths.h"
#include "common/utils.h"
#include "common/string_light.h"

#include "config/feature.h"

#include "drivers/serial.h"
#include "drivers/time.h"
#include "drivers/osd_symbols.h"

#include "fc/config.h"
#include "fc/fc_core.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"
#include "fc/settings.h"

#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/servos.h"

#include "io/gps.h"
#include "io/ledstrip.h"
#include "io/serial.h"

#include "navigation/navigation.h"
#include "navigation/navigation_private.h"

#include "rx/rx.h"

#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/battery.h"
#include "sensors/boardalignment.h"
#include "sensors/gyro.h"
#include "sensors/pitotmeter.h"
#include "sensors/diagnostics.h"
#include "sensors/sensors.h"
#include "sensors/temperature.h"
#include "sensors/esc_sensor.h"

#include "telemetry/mavlink.h"
#include "telemetry/telemetry.h"

// mavlink library uses unnames unions that's causes GCC to complain if -Wpedantic is used
// until this is resolved in mavlink library - ignore -Wpedantic for mavlink code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "common/mavlink.h"
#pragma GCC diagnostic pop

#define TELEMETRY_MAVLINK_PORT_MODE     MODE_RXTX
#define TELEMETRY_MAVLINK_MAXRATE       50
#define TELEMETRY_MAVLINK_DELAY         ((1000 * 1000) / TELEMETRY_MAVLINK_MAXRATE)

// according to __mavlink_battery_status_t.voltages
#define MAVLINK_MSG_ID_BATTERY_STATUS_MAX_CELLS 10

#define STATUS_TEXT_LENGTH 50
#define STATUS_TEXT_ALTERNATING_CHOICES(ms, num_choices) ((millis() / ms) % num_choices)
#define _CONST_STR_SIZE(s) ((sizeof(s)/sizeof(s[0]))-1) // -1 to avoid counting final '\0'
// Wrap all string constants intenteded for display as messages with
// this macro to ensure compile time length validation.
#define STATUS_TEXT_STR(x) ({ \
    STATIC_ASSERT(_CONST_STR_SIZE(x) <= STATUS_TEXT_LENGTH, message_string_ ## __COUNTER__ ## _too_long); \
    x; \
})


/** @brief A mapping of plane flight modes for custom_mode field of heartbeat. */
typedef enum APM_PLANE_MODE
{
   PLANE_MODE_MANUAL=0,
   PLANE_MODE_CIRCLE=1,
   PLANE_MODE_STABILIZE=2,
   PLANE_MODE_TRAINING=3,
   PLANE_MODE_ACRO=4,
   PLANE_MODE_FLY_BY_WIRE_A=5,
   PLANE_MODE_FLY_BY_WIRE_B=6,
   PLANE_MODE_CRUISE=7,
   PLANE_MODE_AUTOTUNE=8,
   PLANE_MODE_AUTO=10,
   PLANE_MODE_RTL=11,
   PLANE_MODE_LOITER=12,
   PLANE_MODE_TAKEOFF=13,
   PLANE_MODE_AVOID_ADSB=14,
   PLANE_MODE_GUIDED=15,
   PLANE_MODE_INITIALIZING=16,
   PLANE_MODE_QSTABILIZE=17,
   PLANE_MODE_QHOVER=18,
   PLANE_MODE_QLOITER=19,
   PLANE_MODE_QLAND=20,
   PLANE_MODE_QRTL=21,
   PLANE_MODE_QAUTOTUNE=22,
   PLANE_MODE_ENUM_END=23,
} APM_PLANE_MODE;

/** @brief A mapping of copter flight modes for custom_mode field of heartbeat. */
typedef enum APM_COPTER_MODE
{
   COPTER_MODE_STABILIZE=0,
   COPTER_MODE_ACRO=1,
   COPTER_MODE_ALT_HOLD=2,
   COPTER_MODE_AUTO=3,
   COPTER_MODE_GUIDED=4,
   COPTER_MODE_LOITER=5,
   COPTER_MODE_RTL=6,
   COPTER_MODE_CIRCLE=7,
   COPTER_MODE_LAND=9,
   COPTER_MODE_DRIFT=11,
   COPTER_MODE_SPORT=13,
   COPTER_MODE_FLIP=14,
   COPTER_MODE_AUTOTUNE=15,
   COPTER_MODE_POSHOLD=16,
   COPTER_MODE_BRAKE=17,
   COPTER_MODE_THROW=18,
   COPTER_MODE_AVOID_ADSB=19,
   COPTER_MODE_GUIDED_NOGPS=20,
   COPTER_MODE_SMART_RTL=21,
   COPTER_MODE_ENUM_END=22,
} APM_COPTER_MODE;

static serialPort_t *mavlinkPort = NULL;
static serialPortConfig_t *portConfig;

static bool mavlinkTelemetryEnabled =  false;
static portSharing_e mavlinkPortSharing;

/* MAVLink datastream rates in Hz */
static uint8_t mavRates[] = {
    [MAV_DATA_STREAM_EXTENDED_STATUS] = 2,      // 2Hz
    [MAV_DATA_STREAM_RC_CHANNELS] = 5,          // 5Hz
    [MAV_DATA_STREAM_POSITION] = 2,             // 2Hz
    [MAV_DATA_STREAM_EXTRA1] = 10,              // 10Hz
    [MAV_DATA_STREAM_EXTRA2] = 2,               // 2Hz
    [MAV_DATA_STREAM_EXTRA3] = 1                // 1Hz
};

#define MAXSTREAMS (sizeof(mavRates) / sizeof(mavRates[0]))

static timeUs_t lastMavlinkMessage = 0;
static uint8_t mavTicks[MAXSTREAMS];
static mavlink_message_t mavSendMsg;
static mavlink_message_t mavRecvMsg;
static mavlink_status_t mavRecvStatus;

static uint8_t mavSystemId = 1;
static uint8_t mavComponentId = MAV_COMP_ID_SYSTEM_CONTROL;

APM_COPTER_MODE inavToArduCopterMap(flightModeForTelemetry_e flightMode)
{
    switch (flightMode)
    {
        case FLM_ACRO:          return COPTER_MODE_ACRO;
        case FLM_ACRO_AIR:      return COPTER_MODE_ACRO;
        case FLM_ANGLE:         return COPTER_MODE_STABILIZE;
        case FLM_HORIZON:       return COPTER_MODE_STABILIZE;
        case FLM_ALTITUDE_HOLD: return COPTER_MODE_ALT_HOLD;
        case FLM_POSITION_HOLD: return COPTER_MODE_POSHOLD;
        case FLM_RTH:           return COPTER_MODE_RTL;
        case FLM_MISSION:       return COPTER_MODE_AUTO;
        case FLM_LAUNCH:        return COPTER_MODE_THROW;
        case FLM_FAILSAFE:      return COPTER_MODE_RTL;
        default:                return COPTER_MODE_ENUM_END;
    }
}

APM_PLANE_MODE inavToArduPlaneMap(flightModeForTelemetry_e flightMode)
{
    switch (flightMode)
    {
        case FLM_MANUAL:        return PLANE_MODE_MANUAL;
        case FLM_ACRO:          return PLANE_MODE_ACRO;
        case FLM_ACRO_AIR:      return PLANE_MODE_ACRO;
        case FLM_ANGLE:         return PLANE_MODE_FLY_BY_WIRE_A;
        case FLM_HORIZON:       return PLANE_MODE_STABILIZE;
        case FLM_ALTITUDE_HOLD: return PLANE_MODE_FLY_BY_WIRE_B;
        case FLM_POSITION_HOLD: return PLANE_MODE_LOITER;
        case FLM_RTH:           return PLANE_MODE_RTL;
        case FLM_MISSION:       return PLANE_MODE_AUTO;
        case FLM_CRUISE:        return PLANE_MODE_CRUISE;
        case FLM_LAUNCH:        return PLANE_MODE_TAKEOFF;
        case FLM_FAILSAFE:      return PLANE_MODE_RTL;
        default:                return PLANE_MODE_ENUM_END;
    }
}

// Used twice, make sure it's exactly the same string
// to save some memory
#define RC_RX_LINK_LOST_MSG "!RC RX LINK LOST!"

static const char * mavlinkArmingDisabledReasonMessage(void)
{
    switch (isArmingDisabledReason()) {
        case ARMING_DISABLED_FAILSAFE_SYSTEM:
            // See handling of FAILSAFE_RX_LOSS_MONITORING in failsafe.c
            if (failsafePhase() == FAILSAFE_RX_LOSS_MONITORING) {
                if (failsafeIsReceivingRxData()) {
                    // If we're not using sticks, it means the ARM switch
                    // hasn't been off since entering FAILSAFE_RX_LOSS_MONITORING
                    // yet
                    return STATUS_TEXT_STR("TURN ARM SWITCH OFF");
                }
                // Not receiving RX data
                return STATUS_TEXT_STR(RC_RX_LINK_LOST_MSG);
            }
            return STATUS_TEXT_STR("DISABLED BY FAILSAFE");
        case ARMING_DISABLED_NOT_LEVEL:
            return STATUS_TEXT_STR("AIRCRAFT IS NOT LEVEL");
        case ARMING_DISABLED_SENSORS_CALIBRATING:
            return STATUS_TEXT_STR("SENSORS CALIBRATING");
        case ARMING_DISABLED_SYSTEM_OVERLOADED:
            return STATUS_TEXT_STR("SYSTEM OVERLOADED");
        case ARMING_DISABLED_NAVIGATION_UNSAFE:
#if defined(USE_NAV)
            // Check the exact reason
            switch (navigationIsBlockingArming(NULL)) {
                case NAV_ARMING_BLOCKER_NONE:
                    break;
                case NAV_ARMING_BLOCKER_MISSING_GPS_FIX:
                    return STATUS_TEXT_STR("WAITING FOR GPS FIX");
                case NAV_ARMING_BLOCKER_NAV_IS_ALREADY_ACTIVE:
                    return STATUS_TEXT_STR("DISABLE NAVIGATION FIRST");
                case NAV_ARMING_BLOCKER_FIRST_WAYPOINT_TOO_FAR:
                    return STATUS_TEXT_STR("FIRST WAYPOINT IS TOO FAR");
                case NAV_ARMING_BLOCKER_JUMP_WAYPOINT_ERROR:
                    return STATUS_TEXT_STR("JUMP WAYPOINT MISCONFIGURED");
            }
#endif
            break;
        case ARMING_DISABLED_COMPASS_NOT_CALIBRATED:
            return STATUS_TEXT_STR("COMPASS NOT CALIBRATED");
        case ARMING_DISABLED_ACCELEROMETER_NOT_CALIBRATED:
            return STATUS_TEXT_STR("ACCELEROMETER NOT CALIBRATED");
        case ARMING_DISABLED_ARM_SWITCH:
            return STATUS_TEXT_STR("DISABLE ARM SWITCH FIRST");
        case ARMING_DISABLED_HARDWARE_FAILURE:
            {
                if (!HW_SENSOR_IS_HEALTHY(getHwGyroStatus())) {
                    return STATUS_TEXT_STR("GYRO FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwAccelerometerStatus())) {
                    return STATUS_TEXT_STR("ACCELEROMETER FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwCompassStatus())) {
                    return STATUS_TEXT_STR("COMPASS FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwBarometerStatus())) {
                    return STATUS_TEXT_STR("BAROMETER FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwGPSStatus())) {
                    return STATUS_TEXT_STR("GPS FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwRangefinderStatus())) {
                    return STATUS_TEXT_STR("RANGE FINDER FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwPitotmeterStatus())) {
                    return STATUS_TEXT_STR("PITOT METER FAILURE");
                }
            }
            return STATUS_TEXT_STR("HARDWARE FAILURE");
        case ARMING_DISABLED_BOXFAILSAFE:
            return STATUS_TEXT_STR("FAILSAFE MODE ENABLED");
        case ARMING_DISABLED_BOXKILLSWITCH:
            return STATUS_TEXT_STR("KILLSWITCH MODE ENABLED");
        case ARMING_DISABLED_RC_LINK:
            return STATUS_TEXT_STR("NO RC LINK");
        case ARMING_DISABLED_THROTTLE:
            return STATUS_TEXT_STR("THROTTLE IS NOT LOW");
        case ARMING_DISABLED_ROLLPITCH_NOT_CENTERED:
            return STATUS_TEXT_STR("ROLLPITCH NOT CENTERED");
        case ARMING_DISABLED_SERVO_AUTOTRIM:
            return STATUS_TEXT_STR("AUTOTRIM IS ACTIVE");
        case ARMING_DISABLED_OOM:
            return STATUS_TEXT_STR("NOT ENOUGH MEMORY");
        case ARMING_DISABLED_INVALID_SETTING:
            return STATUS_TEXT_STR("INVALID SETTING");
        case ARMING_DISABLED_CLI:
            return STATUS_TEXT_STR("CLI IS ACTIVE");
        case ARMING_DISABLED_PWM_OUTPUT_ERROR:
            return STATUS_TEXT_STR("PWM INIT ERROR");
            // Cases without message
        case ARMING_DISABLED_CMS_MENU:
            FALLTHROUGH;
        case ARMING_DISABLED_OSD_MENU:
            FALLTHROUGH;
        case ARMING_DISABLED_ALL_FLAGS:
            FALLTHROUGH;
        case ARMED:
            FALLTHROUGH;
        case WAS_EVER_ARMED:
            break;
    }
    return NULL;
}

static const char * mavlinkFailsafePhaseMessage(void)
{
    // See failsafe.h for each phase explanation
    switch (failsafePhase()) {
#ifdef USE_NAV
        case FAILSAFE_RETURN_TO_HOME:
            // XXX: Keep this in sync with OSD_FLYMODE.
            return STATUS_TEXT_STR("(RTH)");
#endif
        case FAILSAFE_LANDING:
            // This should be considered an emergengy landing
            return STATUS_TEXT_STR("(EMERGENCY LANDING)");
        case FAILSAFE_RX_LOSS_MONITORING:
            // Only reachable from FAILSAFE_LANDED, which performs
            // a disarm. Since aircraft has been disarmed, we no
            // longer show failsafe details.
            FALLTHROUGH;
        case FAILSAFE_LANDED:
            // Very brief, disarms and transitions into
            // FAILSAFE_RX_LOSS_MONITORING. Note that it prevents
            // further rearming via ARMING_DISABLED_FAILSAFE_SYSTEM,
            // so we'll show the user how to re-arm in when
            // that flag is the reason to prevent arming.
            FALLTHROUGH;
        case FAILSAFE_RX_LOSS_IDLE:
            // This only happens when user has chosen NONE as FS
            // procedure. The recovery messages should be enough.
            FALLTHROUGH;
        case FAILSAFE_IDLE:
            // Failsafe not active
            FALLTHROUGH;
        case FAILSAFE_RX_LOSS_DETECTED:
            // Very brief, changes to FAILSAFE_RX_LOSS_RECOVERED
            // or the FS procedure immediately.
            FALLTHROUGH;
        case FAILSAFE_RX_LOSS_RECOVERED:
            // Exiting failsafe
            break;
    }
    return NULL;
}

static const char * mavlinkFailsafeInfoMessage(void)
{
    if (failsafeIsReceivingRxData()) {
        // User must move sticks to exit FS mode
        return STATUS_TEXT_STR("!MOVE STICKS TO EXIT FS!");
    }
    return STATUS_TEXT_STR(RC_RX_LINK_LOST_MSG);
}

static const char * mavlinkNavigationStateMessage(void)
{
    switch (NAV_Status.state) {
        case MW_NAV_STATE_NONE:
            break;
        case MW_NAV_STATE_RTH_START:
            return STATUS_TEXT_STR("STARTING RTH");
        case MW_NAV_STATE_RTH_ENROUTE:
            // TODO: Break this up between climb and head home
            return STATUS_TEXT_STR("EN ROUTE TO HOME");
        case MW_NAV_STATE_HOLD_INFINIT:
            // Used by HOLD flight modes. No information to add.
            break;
        case MW_NAV_STATE_HOLD_TIMED:
            // TODO: Maybe we can display a count down
            return STATUS_TEXT_STR("HOLDING WAYPOINT");
            break;
        case MW_NAV_STATE_WP_ENROUTE:
            // TODO: Show WP number
            return STATUS_TEXT_STR("TO WP");
        case MW_NAV_STATE_PROCESS_NEXT:
            return STATUS_TEXT_STR("PREPARING FOR NEXT WAYPOINT");
        case MW_NAV_STATE_DO_JUMP:
            // Not used
            break;
        case MW_NAV_STATE_LAND_START:
            // Not used
            break;
        case MW_NAV_STATE_EMERGENCY_LANDING:
            return STATUS_TEXT_STR("EMERGENCY LANDING");
        case MW_NAV_STATE_LAND_IN_PROGRESS:
            return STATUS_TEXT_STR("LANDING");
        case MW_NAV_STATE_HOVER_ABOVE_HOME:
            if (STATE(FIXED_WING_LEGACY)) {
                return STATUS_TEXT_STR("LOITERING AROUND HOME");
            }
            return STATUS_TEXT_STR("HOVERING");
        case MW_NAV_STATE_LANDED:
            return STATUS_TEXT_STR("LANDED");
        case MW_NAV_STATE_LAND_SETTLE:
            return STATUS_TEXT_STR("PREPARING TO LAND");
        case MW_NAV_STATE_LAND_START_DESCENT:
            // Not used
            break;
    }
    return NULL;
}

static MAV_SEVERITY getMAVLinkSystemMsg(char* buff)
{
    MAV_SEVERITY severityLevel = MAV_SEVERITY_INFO;
    const char *message = NULL;
    char messageBuf[MAX(SETTING_MAX_NAME_LENGTH, STATUS_TEXT_LENGTH+1)];
    if (ARMING_FLAG(ARMED)) {
        // Aircraft is armed. We might have up to 5
        // messages to show.
        const char *messages[5];
        unsigned messageCount = 0;
        if (FLIGHT_MODE(FAILSAFE_MODE)) {
            // In FS mode while being armed too
            const char *failsafePhaseMessage = mavlinkFailsafePhaseMessage();
            const char *failsafeInfoMessage = mavlinkFailsafeInfoMessage();
            const char *navStateFSMessage = mavlinkNavigationStateMessage();
            if (failsafePhaseMessage) {
                messages[messageCount++] = failsafePhaseMessage;
            }
            if (failsafeInfoMessage) {
                messages[messageCount++] = failsafeInfoMessage;
            }
            if (navStateFSMessage) {
                messages[messageCount++] = navStateFSMessage;
            }
            if (messageCount > 0) {
                message = messages[STATUS_TEXT_ALTERNATING_CHOICES(1000, messageCount)];
                if (message == failsafeInfoMessage) {
                    severityLevel = MAV_SEVERITY_CRITICAL;
                }
            }
        } else {
            if (FLIGHT_MODE(NAV_RTH_MODE) || FLIGHT_MODE(NAV_WP_MODE) || navigationIsExecutingAnEmergencyLanding()) {
                const char *navStateMessage = mavlinkNavigationStateMessage();
                if (navStateMessage) {
                    messages[messageCount++] = navStateMessage;
                }
            } else if (STATE(FIXED_WING_LEGACY) && (navGetCurrentStateFlags() & NAV_CTL_LAUNCH)) {
                    messages[messageCount++] = "AUTOLAUNCH";
            } else {
                if (FLIGHT_MODE(NAV_ALTHOLD_MODE) && !navigationRequiresAngleMode()) {
                    // ALTHOLD might be enabled alongside ANGLE/HORIZON/ACRO
                    // when it doesn't require ANGLE mode (required only in FW
                    // right now). If if requires ANGLE, its display is handled
                    // by OSD_FLYMODE.
                    messages[messageCount++] = "(ALTITUDE HOLD)";
                }
                if (IS_RC_MODE_ACTIVE(BOXAUTOTRIM)) {
                    messages[messageCount++] = "(AUTOTRIM)";
                }
                if (IS_RC_MODE_ACTIVE(BOXAUTOTUNE)) {
                    messages[messageCount++] = "(AUTOTUNE)";
                }
                if (FLIGHT_MODE(HEADFREE_MODE)) {
                    messages[messageCount++] = "(HEADFREE)";
                }
            }
            // Pick one of the available messages. Each message lasts
            // a second.
            if (messageCount > 0) {
                message = messages[STATUS_TEXT_ALTERNATING_CHOICES(1000, messageCount)];
            }
        }
    } else if (ARMING_FLAG(ARMING_DISABLED_ALL_FLAGS)) {
        unsigned invalidIndex;
        // Check if we're unable to arm for some reason
        if (ARMING_FLAG(ARMING_DISABLED_INVALID_SETTING) && !settingsValidate(&invalidIndex)) {
            if (STATUS_TEXT_ALTERNATING_CHOICES(1000, 2) == 0) {
                const setting_t *setting = settingGet(invalidIndex);
                settingGetName(setting, messageBuf);
                for (int ii = 0; messageBuf[ii]; ii++) {
                    messageBuf[ii] = sl_toupper(messageBuf[ii]);
                }
                message = messageBuf;
            } else {
                message = "INVALID SETTING";
                severityLevel = MAV_SEVERITY_WARNING;
            }
        } else {
            if (STATUS_TEXT_ALTERNATING_CHOICES(1000, 2) == 0) {
                message = "UNABLE TO ARM";
                severityLevel = MAV_SEVERITY_WARNING;
            } else {
                // Show the reason for not arming
                message = mavlinkArmingDisabledReasonMessage();
            }
        }
    }

    if (message) {
        int messageLength = strlen(message);
        strncpy(buff, message, STATUS_TEXT_LENGTH);
        // Ensure buff is zero terminated
        buff[messageLength] = '\0';
    }

    return severityLevel;
}


static int mavlinkStreamTrigger(enum MAV_DATA_STREAM streamNum)
{
    uint8_t rate = (uint8_t) mavRates[streamNum];
    if (rate == 0) {
        return 0;
    }

    if (mavTicks[streamNum] == 0) {
        // we're triggering now, setup the next trigger point
        if (rate > TELEMETRY_MAVLINK_MAXRATE) {
            rate = TELEMETRY_MAVLINK_MAXRATE;
        }

        mavTicks[streamNum] = (TELEMETRY_MAVLINK_MAXRATE / rate);
        return 1;
    }

    // count down at TASK_RATE_HZ
    mavTicks[streamNum]--;
    return 0;
}

void freeMAVLinkTelemetryPort(void)
{
    closeSerialPort(mavlinkPort);
    mavlinkPort = NULL;
    mavlinkTelemetryEnabled = false;
}

void initMAVLinkTelemetry(void)
{
    portConfig = findSerialPortConfig(FUNCTION_TELEMETRY_MAVLINK);
    mavlinkPortSharing = determinePortSharing(portConfig, FUNCTION_TELEMETRY_MAVLINK);
}

void configureMAVLinkTelemetryPort(void)
{
    if (!portConfig) {
        return;
    }

    baudRate_e baudRateIndex = portConfig->telemetry_baudrateIndex;
    if (baudRateIndex == BAUD_AUTO) {
        // default rate for minimOSD
        baudRateIndex = BAUD_57600;
    }

    mavlinkPort = openSerialPort(portConfig->identifier, FUNCTION_TELEMETRY_MAVLINK, NULL, NULL, baudRates[baudRateIndex], TELEMETRY_MAVLINK_PORT_MODE, SERIAL_NOT_INVERTED);

    if (!mavlinkPort) {
        return;
    }

    mavlinkTelemetryEnabled = true;
}

static void configureMAVLinkStreamRates(void)
{
    mavRates[MAV_DATA_STREAM_EXTENDED_STATUS] = telemetryConfig()->mavlink.extended_status_rate;
    mavRates[MAV_DATA_STREAM_RC_CHANNELS] = telemetryConfig()->mavlink.rc_channels_rate;
    mavRates[MAV_DATA_STREAM_POSITION] = telemetryConfig()->mavlink.position_rate;
    mavRates[MAV_DATA_STREAM_EXTRA1] = telemetryConfig()->mavlink.extra1_rate;
    mavRates[MAV_DATA_STREAM_EXTRA2] = telemetryConfig()->mavlink.extra2_rate;
    mavRates[MAV_DATA_STREAM_EXTRA3] = telemetryConfig()->mavlink.extra3_rate;
}

void checkMAVLinkTelemetryState(void)
{
    bool newTelemetryEnabledValue = telemetryDetermineEnabledState(mavlinkPortSharing);

    if (newTelemetryEnabledValue == mavlinkTelemetryEnabled) {
        return;
    }

    if (newTelemetryEnabledValue) {
        configureMAVLinkTelemetryPort();
        configureMAVLinkStreamRates();
    } else
        freeMAVLinkTelemetryPort();
}

static void mavlinkSendMessage(void)
{
    uint8_t mavBuffer[MAVLINK_MAX_PACKET_LEN];
    int msgLength = mavlink_msg_to_send_buffer(mavBuffer, &mavSendMsg);

    for (int i = 0; i < msgLength; i++) {
        serialWrite(mavlinkPort, mavBuffer[i]);
    }
}

void mavlinkSendSystemStatus(void)
{
    uint32_t onboardControlAndSensors = 35843;

    /*
    onboard_control_sensors_present Bitmask
    fedcba9876543210
    1000110000000011    For all   = 35843
    0001000000000100    With Mag  = 4100
    0010000000001000    With Baro = 8200
    0100000000100000    With GPS  = 16416
    0000001111111111
    */

    if (sensors(SENSOR_MAG))  onboardControlAndSensors |=  4100;
    if (sensors(SENSOR_BARO)) onboardControlAndSensors |=  8200;
    if (sensors(SENSOR_GPS))  onboardControlAndSensors |= 16416;

    mavlink_msg_sys_status_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // onboard_control_sensors_present Bitmask showing which onboard controllers and sensors are present.
        //Value of 0: not present. Value of 1: present. Indices: 0: 3D gyro, 1: 3D acc, 2: 3D mag, 3: absolute pressure,
        // 4: differential pressure, 5: GPS, 6: optical flow, 7: computer vision position, 8: laser based position,
        // 9: external ground-truth (Vicon or Leica). Controllers: 10: 3D angular rate control 11: attitude stabilization,
        // 12: yaw position, 13: z/altitude control, 14: x/y position control, 15: motor outputs / control
        onboardControlAndSensors,
        // onboard_control_sensors_enabled Bitmask showing which onboard controllers and sensors are enabled
        onboardControlAndSensors,
        // onboard_control_sensors_health Bitmask showing which onboard controllers and sensors are operational or have an error.
        onboardControlAndSensors & 1023,
        // load Maximum usage in percent of the mainloop time, (0%: 0, 100%: 1000) should be always below 1000
        0,
        // voltage_battery Battery voltage, in millivolts (1 = 1 millivolt)
        feature(FEATURE_VBAT) ? getBatteryVoltage() * 10 : 0,
        // current_battery Battery current, in 10*milliamperes (1 = 10 milliampere), -1: autopilot does not measure the current
        isAmperageConfigured() ? getAmperage() : -1,
        // battery_remaining Remaining battery energy: (0%: 0, 100%: 100), -1: autopilot estimate the remaining battery
        feature(FEATURE_VBAT) ? calculateBatteryPercentage() : 100,
        // drop_rate_comm Communication drops in percent, (0%: 0, 100%: 10'000), (UART, I2C, SPI, CAN), dropped packets on all links (packets that were corrupted on reception on the MAV)
        0,
        // errors_comm Communication errors (UART, I2C, SPI, CAN), dropped packets on all links (packets that were corrupted on reception on the MAV)
        0,
        // errors_count1 Autopilot-specific errors
        0,
        // errors_count2 Autopilot-specific errors
        0,
        // errors_count3 Autopilot-specific errors
        0,
        // errors_count4 Autopilot-specific errors
        0);

    mavlinkSendMessage();
}

void mavlinkSendRCChannelsAndRSSI(void)
{
#define GET_CHANNEL_VALUE(x) ((rxRuntimeConfig.channelCount >= (x + 1)) ? rxGetChannelValue(x) : 0)
    mavlink_msg_rc_channels_raw_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // time_boot_ms Timestamp (milliseconds since system boot)
        millis(),
        // port Servo output port (set of 8 outputs = 1 port). Most MAVs will just use one, but this allows to encode more than 8 servos.
        0,
        // chan1_raw RC channel 1 value, in microseconds
        GET_CHANNEL_VALUE(0),
        // chan2_raw RC channel 2 value, in microseconds
        GET_CHANNEL_VALUE(1),
        // chan3_raw RC channel 3 value, in microseconds
        GET_CHANNEL_VALUE(2),
        // chan4_raw RC channel 4 value, in microseconds
        GET_CHANNEL_VALUE(3),
        // chan5_raw RC channel 5 value, in microseconds
        GET_CHANNEL_VALUE(4),
        // chan6_raw RC channel 6 value, in microseconds
        GET_CHANNEL_VALUE(5),
        // chan7_raw RC channel 7 value, in microseconds
        GET_CHANNEL_VALUE(6),
        // chan8_raw RC channel 8 value, in microseconds
        GET_CHANNEL_VALUE(7),
        // rssi Receive signal strength indicator, 0: 0%, 255: 100%
        scaleRange(getRSSI(), 0, 1023, 0, 255));
#undef GET_CHANNEL_VALUE

    mavlinkSendMessage();
}

#if defined(USE_GPS)
void mavlinkSendPosition(timeUs_t currentTimeUs)
{
    uint8_t gpsFixType = 0;

    if (!sensors(SENSOR_GPS))
        return;

    if (gpsSol.fixType == GPS_NO_FIX)
        gpsFixType = 1;
    else if (gpsSol.fixType == GPS_FIX_2D)
            gpsFixType = 2;
    else if (gpsSol.fixType == GPS_FIX_3D)
            gpsFixType = 3;

    mavlink_msg_gps_raw_int_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // time_usec Timestamp (microseconds since UNIX epoch or microseconds since system boot)
        currentTimeUs,
        // fix_type 0-1: no fix, 2: 2D fix, 3: 3D fix. Some applications will not use the value of this field unless it is at least two, so always correctly fill in the fix.
        gpsFixType,
        // lat Latitude in 1E7 degrees
        gpsSol.llh.lat,
        // lon Longitude in 1E7 degrees
        gpsSol.llh.lon,
        // alt Altitude in 1E3 meters (millimeters) above MSL
        gpsSol.llh.alt * 10,
        // eph GPS HDOP horizontal dilution of position in cm (m*100). If unknown, set to: 65535
        gpsSol.eph,
        // epv GPS VDOP horizontal dilution of position in cm (m*100). If unknown, set to: 65535
        gpsSol.epv,
        // vel GPS ground speed (m/s * 100). If unknown, set to: 65535
        gpsSol.groundSpeed,
        // cog Course over ground (NOT heading, but direction of movement) in degrees * 100, 0.0..359.99 degrees. If unknown, set to: 65535
        gpsSol.groundCourse * 10,
        // satellites_visible Number of satellites visible. If unknown, set to 255
        gpsSol.numSat);

    mavlinkSendMessage();

    // Global position
    mavlink_msg_global_position_int_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // time_usec Timestamp (microseconds since UNIX epoch or microseconds since system boot)
        currentTimeUs,
        // lat Latitude in 1E7 degrees
        gpsSol.llh.lat,
        // lon Longitude in 1E7 degrees
        gpsSol.llh.lon,
        // alt Altitude in 1E3 meters (millimeters) above MSL
        gpsSol.llh.alt * 10,
        // relative_alt Altitude above ground in meters, expressed as * 1000 (millimeters)
#if defined(USE_NAV)
        getEstimatedActualPosition(Z) * 10,
#else
        gpsSol.llh.alt * 10,
#endif
        // Ground X Speed (Latitude), expressed as m/s * 100
        0,
        // Ground Y Speed (Longitude), expressed as m/s * 100
        0,
        // Ground Z Speed (Altitude), expressed as m/s * 100
        0,
        // heading Current heading in degrees, in compass units (0..360, 0=north)
        DECIDEGREES_TO_DEGREES(attitude.values.yaw)
    );

    mavlinkSendMessage();

    mavlink_msg_gps_global_origin_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // latitude Latitude (WGS84), expressed as * 1E7
        GPS_home.lat,
        // longitude Longitude (WGS84), expressed as * 1E7
        GPS_home.lon,
        // altitude Altitude(WGS84), expressed as * 1000
        GPS_home.alt * 10); // FIXME

    mavlinkSendMessage();
}
#endif

void mavlinkSendAttitude(void)
{
    mavlink_msg_attitude_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // time_boot_ms Timestamp (milliseconds since system boot)
        millis(),
        // roll Roll angle (rad)
        DECIDEGREES_TO_RADIANS(attitude.values.roll),
        // pitch Pitch angle (rad)
        DECIDEGREES_TO_RADIANS(-attitude.values.pitch),
        // yaw Yaw angle (rad)
        DECIDEGREES_TO_RADIANS(attitude.values.yaw),
        // rollspeed Roll angular speed (rad/s)
        0,
        // pitchspeed Pitch angular speed (rad/s)
        0,
        // yawspeed Yaw angular speed (rad/s)
        0);

    mavlinkSendMessage();
}

void mavlinkSendHUDAndHeartbeat(void)
{
    float mavAltitude = 0;
    float mavGroundSpeed = 0;
    float mavAirSpeed = 0;
    float mavClimbRate = 0;

#if defined(USE_GPS)
    // use ground speed if source available
    if (sensors(SENSOR_GPS)) {
        mavGroundSpeed = gpsSol.groundSpeed / 100.0f;
    }
#endif

#if defined(USE_PITOT)
    if (sensors(SENSOR_PITOT)) {
        mavAirSpeed = pitot.airSpeed / 100.0f;
    }
#endif

    // select best source for altitude
#if defined(USE_NAV)
    mavAltitude = getEstimatedActualPosition(Z) / 100.0f;
    mavClimbRate = getEstimatedActualVelocity(Z) / 100.0f;
#elif defined(USE_GPS)
    if (sensors(SENSOR_GPS)) {
        // No surface or baro, just display altitude above MLS
        mavAltitude = gpsSol.llh.alt;
    }
#endif

    
    int16_t thr = rxGetChannelValue(THROTTLE);
    if (navigationIsControllingThrottle()) {
        thr = rcCommand[THROTTLE];
    }
    mavlink_msg_vfr_hud_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // airspeed Current airspeed in m/s
        mavAirSpeed,
        // groundspeed Current ground speed in m/s
        mavGroundSpeed,
        // heading Current heading in degrees, in compass units (0..360, 0=north)
        DECIDEGREES_TO_DEGREES(attitude.values.yaw),
        // throttle Current throttle setting in integer percent, 0 to 100
        scaleRange(constrain(thr, PWM_RANGE_MIN, PWM_RANGE_MAX), PWM_RANGE_MIN, PWM_RANGE_MAX, 0, 100),
        // alt Current altitude (MSL), in meters, if we have surface or baro use them, otherwise use GPS (less accurate)
        mavAltitude,
        // climb Current climb rate in meters/second
        mavClimbRate);

    mavlinkSendMessage();


    uint8_t mavModes = MAV_MODE_FLAG_MANUAL_INPUT_ENABLED | MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    if (ARMING_FLAG(ARMED))
        mavModes |= MAV_MODE_FLAG_SAFETY_ARMED;

    uint8_t mavSystemType;
    switch (mixerConfig()->platformType)
    {
        case PLATFORM_MULTIROTOR:
            mavSystemType = MAV_TYPE_QUADROTOR;
            break;
        case PLATFORM_TRICOPTER:
            mavSystemType = MAV_TYPE_TRICOPTER;
            break;
        case PLATFORM_AIRPLANE:
            mavSystemType = MAV_TYPE_FIXED_WING;
            break;
        case PLATFORM_ROVER:
            mavSystemType = MAV_TYPE_GROUND_ROVER;
            break;
        case PLATFORM_BOAT:
            mavSystemType = MAV_TYPE_SURFACE_BOAT;
            break;
        case PLATFORM_HELICOPTER:
            mavSystemType = MAV_TYPE_HELICOPTER;
            break;
        default:
            mavSystemType = MAV_TYPE_GENERIC;
            break;
    }

    flightModeForTelemetry_e flm = getFlightModeForTelemetry();
    uint8_t mavCustomMode;

    if (STATE(FIXED_WING_LEGACY)) {
        mavCustomMode = (uint8_t)inavToArduPlaneMap(flm);
    }
    else {
        mavCustomMode = (uint8_t)inavToArduCopterMap(flm);
    }

    if (flm != FLM_MANUAL) {
        mavModes |= MAV_MODE_FLAG_STABILIZE_ENABLED;
    }
    if (flm == FLM_POSITION_HOLD || flm == FLM_RTH || flm == FLM_MISSION) {
        mavModes |= MAV_MODE_FLAG_GUIDED_ENABLED;
    }

    uint8_t mavSystemState = 0;
    if (ARMING_FLAG(ARMED)) {
        if (failsafeIsActive()) {
            mavSystemState = MAV_STATE_CRITICAL;
        }
        else {
            mavSystemState = MAV_STATE_ACTIVE;
        }
    }
    else if (isCalibrating()) {
        mavSystemState = MAV_STATE_CALIBRATING;
    }
    else {
        mavSystemState = MAV_STATE_STANDBY;
    }

    mavlink_msg_heartbeat_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // type Type of the MAV (quadrotor, helicopter, etc., up to 15 types, defined in MAV_TYPE ENUM)
        mavSystemType,
        // autopilot Autopilot type / class. defined in MAV_AUTOPILOT ENUM
        MAV_AUTOPILOT_GENERIC,
        // base_mode System mode bitfield, see MAV_MODE_FLAGS ENUM in mavlink/include/mavlink_types.h
        mavModes,
        // custom_mode A bitfield for use for autopilot-specific flags.
        mavCustomMode,
        // system_status System status flag, see MAV_STATE ENUM
        mavSystemState);

    mavlinkSendMessage();
}

void mavlinkSendBatteryTemperatureStatusText(void)
{
    uint16_t batteryVoltages[MAVLINK_MSG_ID_BATTERY_STATUS_MAX_CELLS];
    memset(batteryVoltages, UINT16_MAX, sizeof(batteryVoltages));
    if (feature(FEATURE_VBAT)) {
        uint8_t batteryCellCount = getBatteryCellCount();
        if (batteryCellCount > 0) {
            for (int cell=0; (cell < batteryCellCount) && (cell < MAVLINK_MSG_ID_BATTERY_STATUS_MAX_CELLS); cell++) {
                batteryVoltages[cell] = getBatteryAverageCellVoltage() * 10;
            }
        }
        else {
            batteryVoltages[0] = getBatteryVoltage() * 10;
        }
    }
    else {
        batteryVoltages[0] = 0;
    }

    mavlink_msg_battery_status_pack(mavSystemId, mavComponentId, &mavSendMsg,
        // id Battery ID
        0,
        // battery_function Function of the battery
        MAV_BATTERY_FUNCTION_UNKNOWN,
        // type Type (chemistry) of the battery
        MAV_BATTERY_TYPE_UNKNOWN,
        // temperature Temperature of the battery in centi-degrees celsius. INT16_MAX for unknown temperature
        INT16_MAX,
        // voltages Battery voltage of cells, in millivolts (1 = 1 millivolt). Cells above the valid cell count for this battery should have the UINT16_MAX value.
        batteryVoltages,
        // current_battery Battery current, in 10*milliamperes (1 = 10 milliampere), -1: autopilot does not measure the current
        isAmperageConfigured() ? getAmperage() : -1,
        // current_consumed Consumed charge, in milliampere hours (1 = 1 mAh), -1: autopilot does not provide mAh consumption estimate
        isAmperageConfigured() ? getMAhDrawn() : -1,
        // energy_consumed Consumed energy, in 100*Joules (intergrated U*I*dt)  (1 = 100 Joule), -1: autopilot does not provide energy consumption estimate
        isAmperageConfigured() ? getMWhDrawn()*36 : -1,
        // battery_remaining Remaining battery energy: (0%: 0, 100%: 100), -1: autopilot does not estimate the remaining battery);
        feature(FEATURE_VBAT) ? calculateBatteryPercentage() : -1);

    mavlinkSendMessage();


    int16_t temperature;
    sensors(SENSOR_BARO) ? getBaroTemperature(&temperature) : getIMUTemperature(&temperature);
    mavlink_msg_scaled_pressure_pack(mavSystemId, mavComponentId, &mavSendMsg,
        millis(),
        0,
        0,
        temperature * 10);

    mavlinkSendMessage();

    char buff[STATUS_TEXT_LENGTH] = {" "};
    MAV_SEVERITY severity = getMAVLinkSystemMsg(buff);
    if (buff[0] != ' ') {
        mavlink_msg_statustext_pack(mavSystemId, mavComponentId, &mavSendMsg,
            (uint8_t)severity,
            buff);

        mavlinkSendMessage();
    }


}

void processMAVLinkTelemetry(timeUs_t currentTimeUs)
{
    // is executed @ TELEMETRY_MAVLINK_MAXRATE rate
    if (mavlinkStreamTrigger(MAV_DATA_STREAM_EXTENDED_STATUS)) {
        mavlinkSendSystemStatus();
    }

    if (mavlinkStreamTrigger(MAV_DATA_STREAM_RC_CHANNELS)) {
        mavlinkSendRCChannelsAndRSSI();
    }

#ifdef USE_GPS
    if (mavlinkStreamTrigger(MAV_DATA_STREAM_POSITION)) {
        mavlinkSendPosition(currentTimeUs);
    }
#endif

    if (mavlinkStreamTrigger(MAV_DATA_STREAM_EXTRA1)) {
        mavlinkSendAttitude();
    }

    if (mavlinkStreamTrigger(MAV_DATA_STREAM_EXTRA2)) {
        mavlinkSendHUDAndHeartbeat();
    }

    if (mavlinkStreamTrigger(MAV_DATA_STREAM_EXTRA3)) {
        mavlinkSendBatteryTemperatureStatusText();
    }

}

static bool handleIncoming_MISSION_CLEAR_ALL(void)
{
    mavlink_mission_clear_all_t msg;
    mavlink_msg_mission_clear_all_decode(&mavRecvMsg, &msg);

    // Check if this message is for us
    if (msg.target_system == mavSystemId) {
        resetWaypointList();
        mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_ACCEPTED);
        mavlinkSendMessage();
        return true;
    }

    return false;
}

// Static state for MISSION UPLOAD transaction (starting with MISSION_COUNT)
static int incomingMissionWpCount = 0;
static int incomingMissionWpSequence = 0;

static bool handleIncoming_MISSION_COUNT(void)
{
    mavlink_mission_count_t msg;
    mavlink_msg_mission_count_decode(&mavRecvMsg, &msg);

    // Check if this message is for us
    if (msg.target_system == mavSystemId) {
        if (msg.count <= NAV_MAX_WAYPOINTS) {
            incomingMissionWpCount = msg.count; // We need to know how many items to request
            incomingMissionWpSequence = 0;
            mavlink_msg_mission_request_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, incomingMissionWpSequence);
            mavlinkSendMessage();
            return true;
        }
        else if (ARMING_FLAG(ARMED)) {
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_ERROR);
            mavlinkSendMessage();
            return true;
        }
        else {
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_NO_SPACE);
            mavlinkSendMessage();
            return true;
        }
    }

    return false;
}

static bool handleIncoming_MISSION_ITEM(void)
{
    mavlink_mission_item_t msg;
    mavlink_msg_mission_item_decode(&mavRecvMsg, &msg);

    // Check if this message is for us
    if (msg.target_system == mavSystemId) {
        // Check supported values first
        if (ARMING_FLAG(ARMED)) {
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_ERROR);
            mavlinkSendMessage();
            return true;
        }

        if ((msg.autocontinue == 0) || (msg.command != MAV_CMD_NAV_WAYPOINT && msg.command != MAV_CMD_NAV_RETURN_TO_LAUNCH)) {
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_UNSUPPORTED);
            mavlinkSendMessage();
            return true;
        }

        if ((msg.frame != MAV_FRAME_GLOBAL_RELATIVE_ALT) && !(msg.frame == MAV_FRAME_MISSION && msg.command == MAV_CMD_NAV_RETURN_TO_LAUNCH)) {
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_UNSUPPORTED_FRAME);
            mavlinkSendMessage();
            return true;
        }

        if (msg.seq == incomingMissionWpSequence) {
            incomingMissionWpSequence++;

            navWaypoint_t wp;
            wp.action = (msg.command == MAV_CMD_NAV_RETURN_TO_LAUNCH) ? NAV_WP_ACTION_RTH : NAV_WP_ACTION_WAYPOINT;
            wp.lat = (int32_t)(msg.x * 1e7f);
            wp.lon = (int32_t)(msg.y * 1e7f);
            wp.alt = msg.z * 100.0f;
            wp.p1 = 0;
            wp.p2 = 0;
            wp.p3 = 0;
            wp.flag = (incomingMissionWpSequence >= incomingMissionWpCount) ? NAV_WP_FLAG_LAST : 0;

            setWaypoint(incomingMissionWpSequence, &wp);

            if (incomingMissionWpSequence >= incomingMissionWpCount) {
                if (isWaypointListValid()) {
                    mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_ACCEPTED);
                    mavlinkSendMessage();
                }
                else {
                    mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_INVALID);
                    mavlinkSendMessage();
                }
            }
            else {
                mavlink_msg_mission_request_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, incomingMissionWpSequence);
                mavlinkSendMessage();
            }
        }
        else {
            // Wrong sequence number received
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_INVALID_SEQUENCE);
            mavlinkSendMessage();
        }

        return true;
    }

    return false;
}

static bool handleIncoming_MISSION_REQUEST_LIST(void)
{
    mavlink_mission_request_list_t msg;
    mavlink_msg_mission_request_list_decode(&mavRecvMsg, &msg);

    // Check if this message is for us
    if (msg.target_system == mavSystemId) {
        mavlink_msg_mission_count_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, getWaypointCount());
        mavlinkSendMessage();
        return true;
    }

    return false;
}

static bool handleIncoming_MISSION_REQUEST(void)
{
    mavlink_mission_request_t msg;
    mavlink_msg_mission_request_decode(&mavRecvMsg, &msg);

    // Check if this message is for us
    if (msg.target_system == mavSystemId) {
        int wpCount = getWaypointCount();

        if (msg.seq < wpCount) {
            navWaypoint_t wp;
            getWaypoint(msg.seq + 1, &wp);

            mavlink_msg_mission_item_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid,
                        msg.seq,
                        wp.action == NAV_WP_ACTION_RTH ? MAV_FRAME_MISSION : MAV_FRAME_GLOBAL_RELATIVE_ALT,
                        wp.action == NAV_WP_ACTION_RTH ? MAV_CMD_NAV_RETURN_TO_LAUNCH : MAV_CMD_NAV_WAYPOINT,
                        0,
                        1,
                        0, 0, 0, 0,
                        wp.lat / 1e7f,
                        wp.lon / 1e7f,
                        wp.alt / 100.0f);
            mavlinkSendMessage();
        }
        else {
            mavlink_msg_mission_ack_pack(mavSystemId, mavComponentId, &mavSendMsg, mavRecvMsg.sysid, mavRecvMsg.compid, MAV_MISSION_INVALID_SEQUENCE);
            mavlinkSendMessage();
        }

        return true;
    }

    return false;
}

static bool processMAVLinkIncomingTelemetry(void)
{
    while (serialRxBytesWaiting(mavlinkPort) > 0) {
        // Limit handling to one message per cycle
        char c = serialRead(mavlinkPort);
        uint8_t result = mavlink_parse_char(0, c, &mavRecvMsg, &mavRecvStatus);
        if (result == MAVLINK_FRAMING_OK) {
            switch (mavRecvMsg.msgid) {
                case MAVLINK_MSG_ID_HEARTBEAT:
                    break;
                case MAVLINK_MSG_ID_MISSION_CLEAR_ALL:
                    return handleIncoming_MISSION_CLEAR_ALL();
                case MAVLINK_MSG_ID_MISSION_COUNT:
                    return handleIncoming_MISSION_COUNT();
                case MAVLINK_MSG_ID_MISSION_ITEM:
                    return handleIncoming_MISSION_ITEM();
                case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
                    return handleIncoming_MISSION_REQUEST_LIST();
                case MAVLINK_MSG_ID_MISSION_REQUEST:
                    return handleIncoming_MISSION_REQUEST();
                default:
                    return false;
            }
        }
    }

    return false;
}

void handleMAVLinkTelemetry(timeUs_t currentTimeUs)
{
    static bool incomingRequestServed;

    if (!mavlinkTelemetryEnabled) {
        return;
    }

    if (!mavlinkPort) {
        return;
    }

    // If we did serve data on incoming request - skip next scheduled messages batch to avoid link clogging
    if (processMAVLinkIncomingTelemetry()) {
        incomingRequestServed = true;
    }

    if ((currentTimeUs - lastMavlinkMessage) >= TELEMETRY_MAVLINK_DELAY) {
        // Only process scheduled data if we didn't serve any incoming request this cycle
        if (!incomingRequestServed) {
            processMAVLinkTelemetry(currentTimeUs);
        }
        lastMavlinkMessage = currentTimeUs;
        incomingRequestServed = false;
    }


}

#endif
