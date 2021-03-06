/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie Firmware
 *
 * Copyright (C) 2011-2012 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */
#include "stm32f10x_conf.h"
#include "FreeRTOS.h"
#include "task.h"

#include "math.h"

#include "system.h"
#include "pm.h"
#include "stabilizer.h"
#include "commander.h"
#include "controller.h"
#include "sensfusion6.h"
#include "imu.h"
#include "motors.h"
#include "log.h"
#include "pid.h"
#include "ledseq.h"
#include "param.h"
#include "ms5611.h"

#undef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#undef min
#define min(a,b) ((a) < (b) ? (a) : (b))

/**
 * Defines in what divided update rate should the attitude
 * control loop run relative the rate control loop.
 */
#define ATTITUDE_UPDATE_RATE_DIVIDER  2
#define FUSION_UPDATE_DT  (float)(1.0 / (IMU_UPDATE_FREQ / ATTITUDE_UPDATE_RATE_DIVIDER)) // 250hz
// Barometer/ Altitude hold stuff
#define ALTHOLD_UPDATE_RATE_DIVIDER  5 // 500hz/5 = 100hz for barometer measurements
#define ALTHOLD_UPDATE_DT  (float)(1.0 / (IMU_UPDATE_FREQ / ALTHOLD_UPDATE_RATE_DIVIDER))   // 500hz
static Axis3f gyro; // Gyro axis data in deg/s
static Axis3f acc;  // Accelerometer axis data in mG
static Axis3f mag;  // Magnetometer axis data in testla

static float eulerRollActual;
static float eulerPitchActual;
static float eulerYawActual;
static float eulerRollDesired;
static float eulerPitchDesired;
static float eulerYawDesired;
static float rollRateDesired;
static float pitchRateDesired;
static float yawRateDesired;

// Baro variables
static float temperature; // temp from barometer
static float pressure;    // pressure from barometer
static float estimatedAltitude;     // smoothed asl
static float aslRaw;  // raw asl
static float aslLong; // long term asl

// Altitude hold variables
static PidObject altHoldPID; // Used for altitute hold mode. I gets reset when the bat status changes
bool altHold = false;          // Currently in altitude hold mode
bool setAltHold = false;      // Hover mode has just been activated
static float accWZ = 0.0;
static float accMAG = 0.0;
static float vSpeed = 0.0; // Vertical speed (world frame) integrated from vertical acceleration
static float vSpeedComp = 0.0; // Compensated vertical (world frame) speed with controller
static float altHoldErr;                       // Different between target and current altitude

static PidObject hoverPID; // Used for hovering. I Reset on battery change status


// Altitude hold & Baro Params
static float altEstKp1 = 0.45; //PI Observer velocity gain
static float altEstKp2 = 1.0;  //PI Observer positional gain
static float altEstKi  = 0.001; //PI Observer integral gain (bias cancellation)
// PID gain constants, used every time we reinitialize the PID controller
static float altHoldKp = 1.0;
static float altHoldKi = 0.0;
static float altHoldKd = 0.0;
// Hover PID parameters
static float hoverKp = 0.1;
static float hoverKi = 0.18;
static float hoverKd = 0.1;
static float altHoldChange = 0;     // Change in target altitude
static float altHoldTarget = -1;    // Target altitude
static float altHoldErrMax = 1.0; // max cap on current estimated altitude vs target altitude in meters
static float altHoldChange_SENS = 200; // sensitivity of target altitude change (thrust input control) while hovering. Lower = more sensitive & faster changes
static float pidAslFac = 13000; // relates meters asl to thrust
static float pidAlpha = 0.8;   // PID Smoothing //TODO: shouldnt need to do this
static float vAccDeadband = 0.07;  // Vertical acceleration deadband
static float vSpeedASLDeadband = 0.005; // Vertical speed based on barometer readings deadband
static float vSpeedLimit = 5.0;  // (m/s) used to constrain vertical velocity
static float errDeadband = 0.00;  // error (target - altitude) deadband
static uint16_t altHoldMinThrust = 00000; // minimum hover thrust - not used yet
static uint16_t altHoldBaseThrust = 43000; // approximate throttle needed when in perfect hover. More weight/older battery can use a higher value
static uint16_t altHoldMaxThrust = 60000; // max altitude hold thrust

RPYType rollType;
RPYType pitchType;
RPYType yawType;

uint16_t actuatorThrust;
int16_t actuatorRoll;
int16_t actuatorPitch;
int16_t actuatorYaw;

uint32_t motorPowerM4;
uint32_t motorPowerM2;
uint32_t motorPowerM1;
uint32_t motorPowerM3;

static bool isInit;

static void stabilizerAltHoldUpdate();
static void distributePower(const uint16_t thrust, const int16_t roll, const int16_t pitch,
		const int16_t yaw);
static uint16_t limitThrust(int32_t value);
static void stabilizerTask(void* param);
static float constrain(float value, const float minVal, const float maxVal);
static float deadband(float value, const float threshold);

void stabilizerInit(void) {
	if (isInit)
		return;

	motorsInit();
	imu6Init();
	sensfusion6Init();
	controllerInit();

	rollRateDesired = 0;
	pitchRateDesired = 0;
	yawRateDesired = 0;

	xTaskCreate(stabilizerTask, (const signed char * const )"STABILIZER",
			2*configMINIMAL_STACK_SIZE, NULL, /*Piority*/2, NULL);

	isInit = TRUE;
}

bool stabilizerTest(void) {
	bool pass = true;

	pass &= motorsTest();
	pass &= imu6Test();
	pass &= sensfusion6Test();
	pass &= controllerTest();

	return pass;
}

static void stabilizerTask(void* param) {
	uint32_t attitudeCounter = 0;
	uint32_t altHoldCounter = 0;
	uint32_t lastWakeTime;

	vTaskSetApplicationTaskTag(0, (void*) TASK_STABILIZER_ID_NBR);

	//Wait for the system to be fully started to start stabilization loop
	systemWaitStart();

	lastWakeTime = xTaskGetTickCount();

	while (1) {
		vTaskDelayUntil(&lastWakeTime, F2T(IMU_UPDATE_FREQ)); // 500Hz

		// Magnetometer not yet used more then for logging.
		imu9Read(&gyro, &acc, &mag);

		if (imu6IsCalibrated()) {
			commanderGetRPY(&eulerRollDesired, &eulerPitchDesired, &eulerYawDesired);
			commanderGetRPYType(&rollType, &pitchType, &yawType);

			// 250HZ
			if (++attitudeCounter >= ATTITUDE_UPDATE_RATE_DIVIDER) {
				sensfusion6UpdateQ(gyro.x, gyro.y, gyro.z, acc.x, acc.y, acc.z, FUSION_UPDATE_DT);
				sensfusion6GetEulerRPY(&eulerRollActual, &eulerPitchActual, &eulerYawActual);

				accWZ = sensfusion6GetAccZWithoutGravity(acc.x, acc.y, acc.z);
				accMAG = (acc.x * acc.x) + (acc.y * acc.y) + (acc.z * acc.z);
				// Estimate speed from acc (drifts)
				vSpeed += deadband(accWZ, vAccDeadband) * FUSION_UPDATE_DT;

				controllerCorrectAttitudePID(eulerRollActual, eulerPitchActual, eulerYawActual,
						eulerRollDesired, eulerPitchDesired, -eulerYawDesired, &rollRateDesired,
						&pitchRateDesired, &yawRateDesired);
				attitudeCounter = 0;
			}

			// 100HZ
			if (imuHasBarometer() && (++altHoldCounter >= ALTHOLD_UPDATE_RATE_DIVIDER)) {
				stabilizerAltHoldUpdate();
				altHoldCounter = 0;
			}

			if (rollType == RATE) {
				rollRateDesired = eulerRollDesired;
			}
			if (pitchType == RATE) {
				pitchRateDesired = eulerPitchDesired;
			}
			if (yawType == RATE) {
				yawRateDesired = -eulerYawDesired;
			}

			// TODO: Investigate possibility to subtract gyro drift.
			controllerCorrectRatePID(gyro.x, -gyro.y, gyro.z, rollRateDesired, pitchRateDesired,
					yawRateDesired);

			controllerGetActuatorOutput(&actuatorRoll, &actuatorPitch, &actuatorYaw);

			if (!altHold || !imuHasBarometer()) {
				// Use thrust from controller if not in altitude hold mode
				commanderGetThrust(&actuatorThrust);
			} else {
				// Added so thrust can be set to 0 while in altitude hold mode after disconnect
				commanderWatchdog();
			}

			if (actuatorThrust > 0) {
#if defined(TUNE_ROLL)
				distributePower(actuatorThrust, actuatorRoll, 0, 0);
#elif defined(TUNE_PITCH)
				distributePower(actuatorThrust, 0, actuatorPitch, 0);
#elif defined(TUNE_YAW)
				distributePower(actuatorThrust, 0, 0, -actuatorYaw);
#else
				distributePower(actuatorThrust, actuatorRoll, actuatorPitch, -actuatorYaw);
#endif
			} else {
				distributePower(0, 0, 0, 0);
				controllerResetAllPID();
			}
		}
	}
}

static void stabilizerAltHoldUpdate() {
	// Get the time
	float altitudeError = 0;
	static float altitudeError_i = 0;
	float instAcceleration = 0;
	float deltaVertSpeed = 0;
	static uint32_t timeStart = 0;
	static uint32_t timeCurrent = 0;


	// Get altitude hold commands from pilot
	commanderGetAltHold(&altHold, &setAltHold, &altHoldChange);

	// Get barometer height estimates
	//TODO do the smoothing within getData
	ms5611GetData(&pressure, &temperature, &aslRaw);

	// Compute the altitude
	altitudeError = aslRaw - estimatedAltitude;
	altitudeError_i = fmin(50.0, fmax(-50.0, altitudeError_i + altitudeError));

	instAcceleration = deadband(accWZ, vAccDeadband) * 9.80665 + altitudeError_i * altEstKi;

	deltaVertSpeed = instAcceleration * ALTHOLD_UPDATE_DT + (altEstKp1 * ALTHOLD_UPDATE_DT) * altitudeError;
    estimatedAltitude += (vSpeedComp * 2.0 + deltaVertSpeed) * (ALTHOLD_UPDATE_DT / 2) + (altEstKp2 * ALTHOLD_UPDATE_DT) * altitudeError;
    vSpeedComp += deltaVertSpeed;

	aslLong = estimatedAltitude;		// Override aslLong

	// Estimate vertical speed based on Acc - fused with baro to reduce drift
	vSpeedComp = constrain(vSpeedComp, -vSpeedLimit, vSpeedLimit);

	// Reset Integral gain of PID controller if being charged
	if (!pmIsDischarging()) {
		altHoldPID.integ = 0.0;
	}

	// Altitude hold mode just activated, set target altitude as current altitude. Reuse previous integral term as a starting point
	if (setAltHold) {
		// Set to current altitude
		altHoldTarget = estimatedAltitude;

		// Set the start time
		timeStart = xTaskGetTickCount();
		timeCurrent = 0;

		// Reset PID controller
		pidInit(&altHoldPID, estimatedAltitude, altHoldKp, altHoldKi, altHoldKd, ALTHOLD_UPDATE_DT);

		// Cache last integral term for reuse after pid init
		// const float pre_integral = hoverPID.integ;

		// Reset the PID controller for the hover controller. We want zero vertical velocity
		pidInit(&hoverPID, 0, hoverKp, hoverKi, hoverKd, ALTHOLD_UPDATE_DT);
		pidSetIntegralLimit(&hoverPID, 3);
		// TODO set low and high limits depending on voltage
		// TODO for now just use previous I value and manually set limits for whole voltage range
		//                    pidSetIntegralLimit(&altHoldPID, 12345);
		//                    pidSetIntegralLimitLow(&altHoldPID, 12345);              /


		// hoverPID.integ = pre_integral;
	}

	// In altitude hold mode
	if (altHold) {
		// Get current time
		timeCurrent = xTaskGetTickCount();

		// Update target altitude from joy controller input
		altHoldTarget += altHoldChange / altHoldChange_SENS;
		pidSetDesired(&altHoldPID, altHoldTarget);

		// Compute error (current - target), limit the error
		altHoldErr = constrain(deadband(estimatedAltitude - altHoldTarget, errDeadband), -altHoldErrMax, altHoldErrMax);
		pidSetError(&altHoldPID, -altHoldErr);

		// Get control from PID controller, dont update the error (done above)
		float altHoldPIDVal = pidUpdate(&altHoldPID, estimatedAltitude, false);

		// Get the PID value for the hover
		float hoverPIDVal = pidUpdate(&hoverPID, vSpeedComp, true);

		float thrustValFloat;

		// Use different weights depending on time into altHold mode
		if (timeCurrent > 150) {
			// Compute the mixture between the alt hold and the hover PID
			thrustValFloat = 0.5*hoverPIDVal + 0.5*altHoldPIDVal;
		} else {
			// Compute the mixture between the alt hold and the hover PID
			thrustValFloat = 0.1*hoverPIDVal + 0.9*altHoldPIDVal;
		}
		// float thrustVal = 0.5*hoverPIDVal + 0.5*altHoldPIDVal;
		uint32_t thrustVal = altHoldBaseThrust + (int32_t)(thrustValFloat*pidAslFac);

		// compute new thrust
		actuatorThrust = max(altHoldMinThrust, min(altHoldMaxThrust, limitThrust( thrustVal )));

		// i part should compensate for voltage drop
	} else {
		altHoldTarget = 0.0;
		altHoldErr = 0.0;
	}
}

static void distributePower(const uint16_t thrust, const int16_t roll, const int16_t pitch,
		const int16_t yaw) {
#ifdef QUAD_FORMATION_X
	roll = roll >> 1;
	pitch = pitch >> 1;
	motorPowerM1 = limitThrust(thrust - roll + pitch + yaw);
	motorPowerM2 = limitThrust(thrust - roll - pitch - yaw);
	motorPowerM3 = limitThrust(thrust + roll - pitch + yaw);
	motorPowerM4 = limitThrust(thrust + roll + pitch - yaw);
#else // QUAD_FORMATION_NORMAL
	motorPowerM1 = limitThrust(thrust + pitch + yaw);
	motorPowerM2 = limitThrust(thrust - roll - yaw);
	motorPowerM3 = limitThrust(thrust - pitch + yaw);
	motorPowerM4 = limitThrust(thrust + roll - yaw);
#endif

	motorsSetRatio(MOTOR_M1, motorPowerM1);
	motorsSetRatio(MOTOR_M2, motorPowerM2);
	motorsSetRatio(MOTOR_M3, motorPowerM3);
	motorsSetRatio(MOTOR_M4, motorPowerM4);
}

static uint16_t limitThrust(int32_t value) {
	if (value > UINT16_MAX) {
		value = UINT16_MAX;
	} else if (value < 0) {
		value = 0;
	}

	return (uint16_t) value;
}

// Constrain value between min and max
static float constrain(float value, const float minVal, const float maxVal) {
	return min(maxVal, max(minVal,value));
}

// Deadzone
static float deadband(float value, const float threshold) {
	if (fabs(value) < threshold) {
		value = 0;
	} else if (value > 0) {
		value -= threshold;
	} else if (value < 0) {
		value += threshold;
	}
	return value;
}

LOG_GROUP_START(stabilizer) LOG_ADD(LOG_FLOAT, roll, &eulerRollActual)
LOG_ADD(LOG_FLOAT, pitch, &eulerPitchActual)
LOG_ADD(LOG_FLOAT, yaw, &eulerYawActual)
LOG_ADD(LOG_UINT16, thrust, &actuatorThrust)
LOG_GROUP_STOP(stabilizer)

LOG_GROUP_START(acc) LOG_ADD(LOG_FLOAT, x, &acc.x)
LOG_ADD(LOG_FLOAT, y, &acc.y)
LOG_ADD(LOG_FLOAT, z, &acc.z)
LOG_ADD(LOG_FLOAT, zw, &accWZ)
LOG_ADD(LOG_FLOAT, mag2, &accMAG)
LOG_GROUP_STOP(acc)

LOG_GROUP_START(gyro) LOG_ADD(LOG_FLOAT, x, &gyro.x)
LOG_ADD(LOG_FLOAT, y, &gyro.y)
LOG_ADD(LOG_FLOAT, z, &gyro.z)
LOG_GROUP_STOP(gyro)

LOG_GROUP_START(mag) LOG_ADD(LOG_FLOAT, x, &mag.x)
LOG_ADD(LOG_FLOAT, y, &mag.y)
LOG_ADD(LOG_FLOAT, z, &mag.z)
LOG_GROUP_STOP(mag)

LOG_GROUP_START(motor) LOG_ADD(LOG_INT32, m4, &motorPowerM4)
LOG_ADD(LOG_INT32, m1, &motorPowerM1)
LOG_ADD(LOG_INT32, m2, &motorPowerM2)
LOG_ADD(LOG_INT32, m3, &motorPowerM3)
LOG_GROUP_STOP(motor)

// LOG altitude hold PID controller states
LOG_GROUP_START(vpid)
LOG_ADD(LOG_FLOAT, pid, &altHoldPID)
LOG_ADD(LOG_FLOAT, p, &altHoldPID.outP)
LOG_ADD(LOG_FLOAT, i, &altHoldPID.outI)
LOG_ADD(LOG_FLOAT, d, &altHoldPID.outD)
LOG_GROUP_STOP(vpid)

LOG_GROUP_START(baro)
LOG_ADD(LOG_FLOAT, estimatedAltitude, &estimatedAltitude)
LOG_ADD(LOG_FLOAT, aslRaw, &aslRaw)
LOG_ADD(LOG_FLOAT, aslLong, &aslLong)
LOG_ADD(LOG_FLOAT, temp, &temperature)
LOG_ADD(LOG_FLOAT, pressure, &pressure)
LOG_GROUP_STOP(baro)

LOG_GROUP_START(altHold)
LOG_ADD(LOG_FLOAT, err, &altHoldErr)
LOG_ADD(LOG_FLOAT, target, &altHoldTarget)
LOG_ADD(LOG_FLOAT, zSpeed, &vSpeed)
LOG_ADD(LOG_FLOAT, vSpeed, &vSpeed)
LOG_ADD(LOG_FLOAT, vSpeedComp, &vSpeedComp)
LOG_GROUP_STOP(altHold)

// Params for altitude hold
PARAM_GROUP_START(altHold)
PARAM_ADD(PARAM_FLOAT, errDeadband, &errDeadband)
PARAM_ADD(PARAM_FLOAT, altHoldChangeSens, &altHoldChange_SENS)
PARAM_ADD(PARAM_FLOAT, altHoldErrMax, &altHoldErrMax)
PARAM_ADD(PARAM_FLOAT, altEstKp1, &altEstKp1)
PARAM_ADD(PARAM_FLOAT, altEstKp2, &altEstKp2)
PARAM_ADD(PARAM_FLOAT, altEstKi, &altEstKi)
PARAM_ADD(PARAM_FLOAT, kd, &altHoldKd)
PARAM_ADD(PARAM_FLOAT, ki, &altHoldKi)
PARAM_ADD(PARAM_FLOAT, kp, &altHoldKp)
PARAM_ADD(PARAM_FLOAT, hoverKd, &hoverKd)
PARAM_ADD(PARAM_FLOAT, hoverKi, &hoverKi)
PARAM_ADD(PARAM_FLOAT, hoverKp, &hoverKp)
PARAM_ADD(PARAM_FLOAT, pidAlpha, &pidAlpha)
PARAM_ADD(PARAM_FLOAT, pidAslFac, &pidAslFac)
PARAM_ADD(PARAM_FLOAT, vAccDeadband, &vAccDeadband)
PARAM_ADD(PARAM_FLOAT, vSpeedASLDeadband, &vSpeedASLDeadband)
PARAM_ADD(PARAM_FLOAT, vSpeedLimit, &vSpeedLimit)
PARAM_ADD(PARAM_UINT16, baseThrust, &altHoldBaseThrust)
PARAM_ADD(PARAM_UINT16, maxThrust, &altHoldMaxThrust)
PARAM_ADD(PARAM_UINT16, minThrust, &altHoldMinThrust)
PARAM_GROUP_STOP(altHold)

