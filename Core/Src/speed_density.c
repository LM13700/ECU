/*===========================================================================*
 * File:        speed_density.c
 * Project:     ECU
 * Author:      Mateusz Mroz
 * Date:        20.11.2021
 * Brief:       Module containing speed density method calculations
 *===========================================================================*/

/*===========================================================================*
 *
 * INCLUDE SECTION
 *
 *===========================================================================*/

#include "speed_density.h"

#include "engine_constants.h"
#include "engine_sensors.h"
#include "ignition_driver.h"
#include "injection_driver.h"
#include "tables.h"
#include "string.h"

/*===========================================================================*
 *
 * DEFINES AND MACRO SECTION
 *
 *===========================================================================*/

#define SPDEN_NO_PENDING_CHANNEL           (ENCON_CHANNEL_COUNT)

#define SPDEN_TARGET_AFR                   (14.0F)
/* Density in g/cm^3 */
#define SPDEN_FUEL_DENSITY                 (7.0F)

/* Molar mass in g/mol */
#define SPDEN_AIR_MOLAR_MASS               (28.97F)

/* J/(mol*K) */
#define SPDEN_GAS_CONSTANT                 (8.314F)

#define SPDEN_AIR_MASS(_PRESS_, _TEMP_)    ((_PRESS_) / (_TEMP_) * spden_air_mass_helper)

/*===========================================================================*
 *
 * LOCAL TYPES AND ENUMERATION SECTION
 *
 *===========================================================================*/

typedef enum SpDen_EngineState_Tag
{
    SPDEN_ENGINE_STATE_NOT_RUNNING,
    SPDEN_ENGINE_STATE_CRANKING,
    SPDEN_ENGINE_STATE_RUNING,

    SPDEN_ENGINE_STATE_COUNT
} SpDen_EngineState_T;

/*===========================================================================*
 *
 * GLOBAL VARIABLES AND CONSTANTS SECTION
 *
 *===========================================================================*/

static const float spden_air_mass_helper = (ENCON_CYLINDER_VOLUME_M3 * SPDEN_AIR_MOLAR_MASS) / SPDEN_GAS_CONSTANT;

static volatile EnCon_CylinderChannels_T spden_pending_cylinder_channel;

static SpDen_EngineState_T spden_engine_state;

/* Calculations begin after each cylinder work stroke */
static const float spden_calc_beggining_angles[ENCON_ENGINE_PISTONS_NO] =
{
    UTILS_CIRCULAR_ADDITION(ENCON_ENGINE_FIRST_PISTON_OFFSET, ENCON_ENGINE_EXHAUST_ANGLE,
                            ENCON_ENGINE_FULL_CYCLE_ANGLE),
    UTILS_CIRCULAR_ADDITION(ENCON_ENGINE_SECOND_PISTON_OFFSET, ENCON_ENGINE_EXHAUST_ANGLE,
                            ENCON_ENGINE_FULL_CYCLE_ANGLE),
    UTILS_CIRCULAR_ADDITION(ENCON_ENGINE_THIRD_PISTON_OFFSET, ENCON_ENGINE_EXHAUST_ANGLE,
                            ENCON_ENGINE_FULL_CYCLE_ANGLE)
};

/* Cylinders work TDC angle compared to the first piston TDC angle */
static const float spden_work_tdc_angles[ENCON_ENGINE_PISTONS_NO] =
{
    ENCON_ENGINE_FIRST_PISTON_OFFSET,
    ENCON_ENGINE_SECOND_PISTON_OFFSET,
    ENCON_ENGINE_THIRD_PISTON_OFFSET
};

/* Cylinders intake beggining angle compared to the first piston TDC angle */
static const float spden_intake_beggining_angles[ENCON_ENGINE_PISTONS_NO] =
{
    UTILS_CIRCULAR_ADDITION(ENCON_ENGINE_FIRST_PISTON_OFFSET, ENCON_ENGINE_INTAKE_ANGLE,
                            ENCON_ENGINE_FULL_CYCLE_ANGLE) + ENCON_FUEL_DELIVERY_OFFSET_ANGLE,
    UTILS_CIRCULAR_ADDITION(ENCON_ENGINE_SECOND_PISTON_OFFSET, ENCON_ENGINE_INTAKE_ANGLE,
                            ENCON_ENGINE_FULL_CYCLE_ANGLE) + ENCON_FUEL_DELIVERY_OFFSET_ANGLE,
    UTILS_CIRCULAR_ADDITION(ENCON_ENGINE_THIRD_PISTON_OFFSET, ENCON_ENGINE_INTAKE_ANGLE,
                            ENCON_ENGINE_FULL_CYCLE_ANGLE) + ENCON_FUEL_DELIVERY_OFFSET_ANGLE
};

/*===========================================================================*
 *
 * LOCAL FUNCTION DECLARATION SECTION
 *
 *===========================================================================*/

/*===========================================================================*
 * brief:       Check current engine state
 * param[in]:   None
 * param[out]:  None
 * return:      None
 * details:     This function sets global spden_engine_state variable
 *===========================================================================*/
static void SpDen_CheckCurrentEngineState(void);

/*===========================================================================*
 * brief:       Finds if any of the cylinders needs to be prepared
 * param[in]:   None
 * param[out]:  None
 * return:      EnCon_CylinderChannels_T - return valid channel number, or ENCON_CHANNEL_COUNT
 *              when no action need to be taken
 * details:     This function sets global spden_pending_cylinder_channel variable
 *===========================================================================*/
static EnCon_CylinderChannels_T SpDen_GetPendingChannel(void);

/*===========================================================================*
 * brief:       Calculate fuel injection duration
 * param[in]:   speed - engine speed in RPM
 * param[in]:   pressure - engine absolute pressure in kPa
 * param[out]:  None
 * return:      float - fuel duration pulse in ms
 * details:     None
 *===========================================================================*/
static float SpDen_CalculateFuel(float speed, float pressure);

/*===========================================================================*
 * brief:       Calculate spark angle
 * param[in]:   speed - engine speed in RPM
 * param[in]:   pressure - engine absolute pressure in kPa
 * param[in]:   channel - current engine channel
 * param[out]:  None
 * return:      float - spark fire angle
 * details:     None
 *===========================================================================*/
static float SpDen_CalculateSpark(float speed, float pressure, EnCon_CylinderChannels_T channel);

/*===========================================================================*
 *
 * FUNCTION DEFINITION SECTION
 *
 *===========================================================================*/

/*===========================================================================*
 * Function: SpDen_Init
 *===========================================================================*/
void SpDen_Init(void)
{
    spden_pending_cylinder_channel = SPDEN_NO_PENDING_CHANNEL;
    spden_engine_state = SPDEN_ENGINE_STATE_NOT_RUNNING;
}

/*===========================================================================*
 * Function: SpDen_OnTriggerInterrupt
 *===========================================================================*/
void SpDen_OnTriggerInterrupt(void)
{
    EnCon_CylinderChannels_T channel;
    float engineSpeed;
    float enginePressure;
    float engineAngle;
    float fuelPulseMs;
    float sparkAngle;

    SpDen_CheckCurrentEngineState();

    if (spden_engine_state != SPDEN_ENGINE_STATE_NOT_RUNNING)
    {
        channel = SpDen_GetPendingChannel();

        if (channel != SPDEN_NO_PENDING_CHANNEL)
        {
            engineSpeed = EnCon_GetEngineSpeed();
            enginePressure = EnSens_GetMap();

            fuelPulseMs = SpDen_CalculateFuel(engineSpeed, enginePressure);
            sparkAngle = SpDen_CalculateSpark(engineSpeed, enginePressure, channel);

            DisableIRQ();

            engineAngle = EnCon_GetEngineAngle();
            /* Event timers will be triggered after next interrupt */
            engineAngle += ENCON_ONE_TRIGGER_PULSE_ANGLE;

            InjDrv_PrepareInjectionChannel(channel, spden_intake_beggining_angles[channel], engineAngle, fuelPulseMs);
            IgnDrv_PrepareIgnitionChannel(channel, sparkAngle, engineAngle);
            spden_pending_cylinder_channel = channel;

            EnableIRQ();
        }
    }
}

/*===========================================================================*
 * Function: SpDen_TriggerCallback
 *===========================================================================*/
void SpDen_TriggerCallback(void)
{
    if (spden_pending_cylinder_channel != SPDEN_NO_PENDING_CHANNEL)
    {
        IgnDrv_StartIgnitionModule(spden_pending_cylinder_channel);
        InjDrv_StartInjectionModule(spden_pending_cylinder_channel);
        spden_pending_cylinder_channel = SPDEN_NO_PENDING_CHANNEL;
    }
}

/*===========================================================================*
 *
 * LOCAL FUNCTION DEFINITION SECTION
 *
 *===========================================================================*/

/*===========================================================================*
 * Function: SpDen_CheckCurrentEngineState
 *===========================================================================*/
static void SpDen_CheckCurrentEngineState(void)
{
    float engineSpeed;

    engineSpeed = EnCon_GetEngineSpeed();

    if (ENCON_SPEED_UNKNOWN == engineSpeed)
    {
        spden_engine_state = SPDEN_ENGINE_STATE_NOT_RUNNING;
    }
    else if (engineSpeed >= ENCON_RUNNING_FLOOR_RPM)
    {
        spden_engine_state = SPDEN_ENGINE_STATE_RUNING;
    }
    else if (engineSpeed > ENCON_CRANKING_FLOOR_RPM)
    {
        spden_engine_state = SPDEN_ENGINE_STATE_CRANKING;
    }
    else
    {
        spden_engine_state = SPDEN_ENGINE_STATE_NOT_RUNNING;
    }
}

/*===========================================================================*
 * Function: SpDen_GetPendingChannel
 *===========================================================================*/
static EnCon_CylinderChannels_T SpDen_GetPendingChannel(void)
{
    float engineAngle;
    EnCon_CylinderChannels_T channel;

    engineAngle = EnCon_GetEngineAngle();

    for (channel = 0; channel < ENCON_CHANNEL_COUNT; channel++)
    {
        if (spden_calc_beggining_angles[channel] == engineAngle)
        {
            goto sp_den_get_pending_channel_exit;
        }
    }

    channel = ENCON_CHANNEL_COUNT;

sp_den_get_pending_channel_exit:

    return channel;
}

/*===========================================================================*
 * Function: SpDen_CalculateFuel
 *===========================================================================*/
static float SpDen_CalculateFuel(float speed, float pressure)
{
    float correctionMultiplier;

    correctionMultiplier = 0.0F;

    if (SPDEN_ENGINE_STATE_CRANKING == spden_engine_state)
    {
        correctionMultiplier += ENCON_CRANKING_ENRICHMENT;
    }

    correctionMultiplier += EnSens_GetClt(ENSENS_CLT_RESULT_TYPE_ENRICHEMENT);

    return (Tables_GetSpeedPressureTableValue(TABLES_SPEED_PRESSURE_VE, speed, pressure) * SPDEN_FUEL_DENSITY /
           ((SPDEN_AIR_MASS(EnSens_GetMap(), EnSens_GetIat()) / SPDEN_TARGET_AFR) * ENCON_INJECTOR_FLOW_RATE) +
           ENCON_INJECTOR_DEAD_TIME_MS) * correctionMultiplier;
}

/*===========================================================================*
 * Function: SpDen_CalculateSpark
 *===========================================================================*/
static float SpDen_CalculateSpark(float speed, float pressure, EnCon_CylinderChannels_T channel)
{
    return UTILS_CIRCULAR_DIFFERENCE(spden_work_tdc_angles[channel],
                                     Tables_GetSpeedPressureTableValue(TABLES_SPEED_PRESSURE_SPARK, speed, pressure),
                                     ENCON_ENGINE_FULL_CYCLE_ANGLE);
}


/* end of file */