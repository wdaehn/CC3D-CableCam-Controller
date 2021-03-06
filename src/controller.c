#include "config.h"
#include "protocol.h"
#include "controller.h"
#include "protocol.h"
#include "clock_50Hz.h"
#include "sbus.h"
#include "vesc.h"

extern sbusData_t sbusdata;

void printControlLoop(int16_t input, double speed, double pos, double brakedistance, CONTROLLER_MONITOR_t monitor, uint16_t esc, Endpoints endpoint);
void printPIDMonitor(double e, double y, Endpoints endpoint);
int32_t stickCycle(double pos, double brakedistance);

/*
 * Preserve the previous filtered stick value to calculate the acceleration
 */
int32_t stick_last_value = 0;

double yalt = 0.0f, ealt = 0.0f, esum = 0.0f;

uint32_t possensorduration = 0;
uint32_t last_possensortick = 0;

double speed_current = 0.0f;


int32_t pos_current_old = 0L;
double pos_target = 0.0f, pos_target_old = 0.0f;

uint8_t endpointclicks = 0;
uint16_t lastendpointswitch = 0;
uint8_t lastmodeswitchsetting = 255;

#define Ta  0.02


void setPIDValues(double kp, double ki, double kd)
{
    activesettings.P = kp;
    activesettings.I = ki;
    activesettings.D = kd;
}

void setPValue(double v)
{
    activesettings.P = v;
}
void setIValue(double v)
{
    activesettings.I = v;
}
void setDValue(double v)
{
    activesettings.D = v;
}

double getSpeedPosSensor(void)
{
    if (HAL_GetTick() - last_possensortick > 2000)
    {
        return 0.0f;
    }
    else if (possensorduration == 0)
    {
        return 0.0f;
    }
    else
    {
        /* Speed is the number of signals per 0.02 (=Ta) secs.
         * Hence refactor Ta to millisenconds to match the HAL_GetTick() millisecond time scale.
         * But the encoder is set to by an X4 type, so it updates the position on rising and falling flanks
         * of both channels whereas the interrupt fires on one pin and rising flank only - hence times 4.
         */
        return Ta * 1000.0f * 4.0f / ((double) possensorduration);
    }
}

double getSpeedPosDifference(void)
{
    return speed_current;
}

int32_t getTargetPos(void)
{
    return pos_target;
}


int16_t getStick(void)
{
    return stick_last_value;
}

int32_t getPos(void)
{
    return ENCODER_VALUE;
}

void initController(void)
{
    controllerstatus.safemode = INVALID_RC;
    controllerstatus.monitor = FREE;
}

double abs_d(double v)
{
    if (v<0.0)
    {
        return -v;
    }
    else
    {
        return v;
    }
}

uint16_t getProgrammingSwitch()
{
    return getDuty(activesettings.rc_channel_programming);
}

uint16_t getEndPointSwitch()
{
    return getDuty(activesettings.rc_channel_endpoint);
}

uint16_t getMaxAccelPoti()
{
    return getDuty(activesettings.rc_channel_max_accel);
}

uint16_t getMaxSpeedPoti()
{
    return getDuty(activesettings.rc_channel_max_speed);
}

uint16_t getModeSwitch()
{
    return getDuty(activesettings.rc_channel_mode);
}

/*
 * getStickPositionRaw() returns the position centered around 0 of the stick considering the neutral range.
 * Everything within the neutral range means a stick position of zero, the first value outside the neutral range would be 1.
 * Examples:
 *   sbus reading: 885; neutral: 870..890 --> return 0;
 *   sbus reading: 860; neutral: 870..890 --> return 860-870=-10;
 *
 * Therefore the output is a linear range from min to max with the neutral range eliminated from the value.
 * Also, when the reading is too old, meaning older than 3 seconds, the value is reset to zero as emergency precaution.
 *
 * Just to repeat, getDuty() returns a value like 1200 (neutral for SBUS), this function returns +200 then, assuming neutral is set to +1000.
 */
int16_t getStickPositionRaw()
{
    /*
     * "value" is the duty signal rebased from the stick_neutral_pos to zero.
     * So when getDuty() returns 900 and the stick_neutral_pos is 1000, the value would be -100.
     *
     * Remember that getDuty() itself can return a value of 0 as well in case no valid RC signal was received either
     * or non received for a while.
     */
    int16_t value = getDuty(activesettings.rc_channel_speed) - activesettings.stick_neutral_pos;

    if (value == -activesettings.stick_neutral_pos) // in case getDuty() returned 0 meaning no valid stick position was received...
    {
        // No valid value for that, use Neutral
        return 0;
    }
    else
    {
        /*
         * At the beginning the safemode == INVALID_RC, meaning no valid signal was received ever.
         * If a valid rc signal is received, then the safemode is changed according to the programming switch.
         * There is one more case, we got a valid speed signal but the stick is not in neutral. This is considered
         * an invalid signal as well. At startup the speed signal has to be in idle.
         */
        if ((controllerstatus.safemode == INVALID_RC || controllerstatus.safemode == NOT_NEUTRAL_AT_STARTUP) &&
            (value > activesettings.stick_neutral_range ||
             value < -activesettings.stick_neutral_range))
        {
            if (controllerstatus.safemode == INVALID_RC)
            {
                PrintSerial_string("A valid RC signal with value ", EndPoint_All);
                PrintSerial_int(value, EndPoint_All);
                PrintSerial_string(" received on channel ", EndPoint_All);
                PrintSerial_int(value, EndPoint_All);
                PrintSerial_string(" but the neutral point is ", EndPoint_All);
                PrintSerial_int(activesettings.stick_neutral_pos, EndPoint_All);
                PrintSerial_string("+-", EndPoint_All);
                PrintSerial_int(activesettings.stick_neutral_range, EndPoint_All);
                PrintSerial_string(".\r\nCheck the RC, the channel assignments $i and the input neutral point settings $n", EndPoint_All);

                controllerstatus.safemode = NOT_NEUTRAL_AT_STARTUP; // Statemachine to the next level
            }
            return 0;
        }
    }

    /* Remove the neutral range */
    if (value > activesettings.stick_neutral_range )
    {
        // above idle is just fine
        return value - activesettings.stick_neutral_range;
    }
    else if (value < -activesettings.stick_neutral_range)
    {
        // below negative idle is fine
        return value + activesettings.stick_neutral_range;
    }
    else
    {
        // within neutral range means zero
        return 0;
    }
}


/** \brief The receiver provides a stick position and this function applies filters. Returns the filtered value.
 *
 * Depending on the CableCam mode and the distance to brake various filters are applied.
 * In short these filters are acceleration limiter, speed limiter, engage brake for endpoints.
 *
 * In MODE_PASSTHROUGH none of the filters are used, stick output = input (actually output = input*10).
 *
 * In all other modes at least the acceleration and speed filters are active. These should avoid that the
 * user cranks the stick forward, wheels spinning. Instead the filter applies a ramp, increasing the
 * speed constantly by the defined acceleration factor until the output matches the input or the max speed
 * has been reached. What the values for acceleration and speed actually are depends on the programming switch position.
 * In case the endpoints should be programmed the values are stick_max_accel_safemode and stick_max_speed_safemode.
 * In case of OPERATIONAL it is stick_max_accel and stick_max_speed. (see $a and $v commands).
 *
 * The second type of filter is the endpoint limiter. If the CableCam is in danger to hit the start- or endpoint at the
 * current speed with the stick_max_accel as deceleration, the stick is slowly moved into neutral. The endpoint limiter
 * is obviously engaged in OPERATIONAL mode only, otherwise the end points could not be set to further outside.
 *
 * \param pos double Current position
 * \param brakedistance double Brake distance at current speed
 * \return stick_requested_value int16_t
 *
 */
int32_t stickCycle(double pos, double brakedistance)
{
    /*
     * WATCHOUT, while the stick values are all in us, the value, stick_requested_value, esc_out values are all in 0.1us units.
     * Else the acceleration would not be fine grained enough.
     */
    int16_t tmp = getStickPositionRaw();


    // output = ( (1 - factor) x input^3 ) + ( factor x input )     with input and output between -1 and +1

    double stickpercent = ((double) tmp) / ((double) (activesettings.stick_value_range - activesettings.stick_neutral_range));
    double outputpercent = ((1.0f - activesettings.expo_factor) * stickpercent * stickpercent * stickpercent) + (activesettings.expo_factor * stickpercent);


    int32_t value = ((int32_t) (outputpercent * ((double) (activesettings.stick_value_range - activesettings.stick_neutral_range)))) * ESC_STICK_SCALE;

    int32_t speed = ENCODER_VALUE - pos_current_old; // When the current pos is greater than the previous, speed is positive

    // In passthrough mode the returned value is the raw value
    if (activesettings.mode != MODE_PASSTHROUGH)
    {
        /*
         * In all other modes the accel and speed limiters are turned on
         */
        int32_t maxaccel = activesettings.stick_max_accel;
        int32_t maxspeed = activesettings.stick_max_speed;

        if (controllerstatus.safemode != OPERATIONAL)
        {
            maxaccel = activesettings.stick_max_accel_safemode;
            maxspeed = activesettings.stick_max_speed_safemode;
        }

        int32_t diff = value - stick_last_value;

        /*
         * No matter what the stick value says, the speed is limited to the max range.
         * It is important this comes before the the acceleration limiter as it might be the case
         * the user switched to programming mode running at full speed. This should include the slow
         * decel as well.
         */

        if (diff > maxaccel)
        {
            // Example: last time: 150; now stick: 200 --> diff: 50; max change is 10 per cycle --> new value: 150+10=160 as the limiter kicks in
            value = stick_last_value + maxaccel;
        }
        else if (diff < -maxaccel)
        {
            // Example: last time: 150; now stick: 100 --> diff: -50; max change is 10 per cycle --> new value: 150-10=140 as the limiter kicks in
            value = stick_last_value - maxaccel;
        }

        /*
         * It is important to calculate the new value based on the acceleration first, then we have the new target speed,
         * now it is limited to an absolute value.
         */
        if (value > maxspeed*ESC_STICK_SCALE)
        {
            value = maxspeed*ESC_STICK_SCALE;
        }
        else if (value < -maxspeed*ESC_STICK_SCALE)
        {
            value = -maxspeed*ESC_STICK_SCALE;
        }



        if (controllerstatus.safemode == OPERATIONAL && activesettings.mode != MODE_PASSTHROUGH && activesettings.mode != MODE_LIMITER)
        {
            /*
             * The current status is OPERATIONAL, hence the endpoints are honored.
             *
             * If we would overshoot the end points, then ignore the requested value and reduce the stick position by max_accel.
             */


            /*
             * The pos_start has to be smaller than pos_end always. This is checked in the end_point set logic.
             * However there is a cases where this might not be so:
             * Start and end point had been set but then only the start point is moved.
             */
            if (activesettings.pos_start > activesettings.pos_end)
            {
                double tmp = activesettings.pos_start;
                activesettings.pos_start = activesettings.pos_end;
                activesettings.pos_end = tmp;
            }

            /*
             * One problem is the direction. Once the endpoint was overshot, a stick position driving the cablecam even further over
             * the limit is not allowed. But driving it back between start and end point is fine. But what value of the stick
             * is further over the limit? activesettings.esc_direction tells that.
             *
             * Case 1: start pos_start = 0, stick forward drove cablecam to pos_end +3000. Hence esc_direction = +1
             *         Condition: if value > 0 && pos+brakedistance <= pos_end.
             *                Or: if value > 0 && pos+brakedistance <= pos_end.
             * Case 2: start pos_start = 0, stick forward drove cablecam to pos_end -3000. Hence esc_direction = -1.
             *         Condition: if value < 0 && pos+brakedistance <= pos_end.
             *                Or: if -value > 0 && pos+brakedistance <= pos_end.
             * Case 3: start pos_start = 0, stick reverse drove cablecam to pos_end +3000. Hence esc_direction = -1.
             *         Condition: if value < 0 && pos+brakedistance <= pos_end.
             *                Or: if -value > 0 && pos+brakedistance <= pos_end.
             * Case 4: start pos_start = 0, stick reverse drove cablecam to pos_end -3000. Hence esc_direction = +1.
             *         Condition: if value > 0 && pos+brakedistance <= pos_end.
             *                Or: if value > 0 && pos+brakedistance <= pos_end.
             *
             *
             */
            if (pos + brakedistance >= activesettings.pos_end)
            {
                /*
                 * In case the CableCam will overshoot the end point reduce the speed to zero.
                 * Only if the stick is in the reverse direction already, accept the value. Else you cannot maneuver
                 * back into the safe zone.
                 */
                if (((int32_t) activesettings.esc_direction) * value > 0)
                {
                    value = stick_last_value - (maxaccel * ((int32_t) activesettings.esc_direction)); // reduce speed at full allowed acceleration
                    if (value * ((int16_t) activesettings.esc_direction) < 0) // watch out to not get into reverse.
                    {
                        value = 0;
                    }
                    controllerstatus.monitor = ENDPOINTBRAKE;
                    LED_WARN_ON;
                    /*
                     * Failsafe: In case the endpoint itself had been overshot and stick says to do so further, stop immediately.
                     */
                    if (pos >= activesettings.pos_end)
                    {
                        stick_last_value = 0;
                        return 0;
                    }
                }
                else
                {
                    controllerstatus.monitor = FREE;
                    LED_WARN_OFF;
                }

                /*
                 * Failsafe: No matter what the user did going way over the endpoint at speed causes this function to return a speed=0 request.
                 */
                if (pos + brakedistance >= activesettings.pos_end + activesettings.max_position_error && speed > 0)
                {
                    /*
                     * We are in danger to overshoot the end point by max_position_error because we are moving with a speed > 0 towards
                     * the end point. Hence kick in the emergency brake.
                     */
                    controllerstatus.monitor = EMERGENCYBRAKE;
                    LED_WARN_ON;
                    stick_last_value = value;
                    return 0;
                }
            }


            if (pos - brakedistance <= activesettings.pos_start)
            {
                /*
                 * In case the CableCam will overshoot the start point reduce the speed to zero.
                 * Only if the stick is in the reverse direction already, accept the value. Else you cannot maneuver
                 * back into the safe zone.
                 */
                if (((int32_t) activesettings.esc_direction) * value < 0)
                {
                    value = stick_last_value + (maxaccel * ((int32_t) activesettings.esc_direction)); // reduce speed at full allowed acceleration
                    if (value * ((int32_t) activesettings.esc_direction) > 0) // watch out to not get into reverse.
                    {
                        value = 0;
                    }
                    controllerstatus.monitor = ENDPOINTBRAKE;
                    LED_WARN_ON;
                    /*
                     * Failsafe: In case the endpoint itself had been overshot and stick says to do so further, stop immediately.
                     */
                    if (pos <= activesettings.pos_start)
                    {
                        stick_last_value = 0;
                        return 0;
                    }
                }
                else
                {
                    controllerstatus.monitor = FREE;
                    LED_WARN_OFF;
                }

                /*
                 * Failsafe: No matter what the user did going way over the endpoint at speed causes this function to return a speed=0 request.
                 */
                if (pos - brakedistance <= activesettings.pos_start - activesettings.max_position_error && speed < 0)
                {
                    /*
                     * We are in danger to overshoot the end point by max_position_error because we are moving with a speed > 0 towards
                     * the end point. Hence kick in the emergency brake.
                     */
                    controllerstatus.monitor = EMERGENCYBRAKE;
                    LED_WARN_ON;
                    stick_last_value = value;
                    return 0;
                }
            }
        }

    }
    stick_last_value = value; // store the current effective stick position as value for the next cycle







    /*
     * Evaluate the programming mode switch.
     *
     * A HIGH value is considered normal operation
     * Everything else
     *                  LOW value
     *                  value of neutral
     *                  if the channel is not set at all (value == 0)
     * is considered the programming mode with reduced speeds.
     *
     * The reason for including NEUTRAL as non-operational is because that would indicate this channel is not set properly.
     */
    if (getProgrammingSwitch() > 1200)
    {
        if (controllerstatus.safemode != OPERATIONAL)
        {
            PrintlnSerial_string("Entered OPERATIONAL mode", EndPoint_All);
        }
        controllerstatus.safemode = OPERATIONAL;
    }
    else
    {
        if (controllerstatus.safemode != PROGRAMMING)
        {
            /* We just entered the programing mode, hence wait for the first end point
             * First endpoint is the first click, Second endpoint the second click.
             */
            endpointclicks = 0;
            PrintlnSerial_string("Entered Endpoint Programming mode", EndPoint_All);
        }
        controllerstatus.safemode = PROGRAMMING;
    }




    /*
     * Evaluate the End Point Programming switch
     */
    uint16_t currentendpointswitch = getEndPointSwitch();
    if (currentendpointswitch > 1200 && controllerstatus.safemode == PROGRAMMING && lastendpointswitch <= 1200 && lastendpointswitch != 0)
    {
        int32_t pos = ENCODER_VALUE;
        if (endpointclicks == 0)
        {
            activesettings.pos_start = pos;
            endpointclicks = 1;
            PrintlnSerial_string("Point 1 set", EndPoint_All);
        }
        else
        {
            /*
             * Subsequent selections of the endpoint just moves the 2nd Endpoint around. Else there
             * is the danger the user selects point 1 at zero, then travels to 1000 and selects that as endpoint. Then clicks again
             * and therefore the range is from 1000 to 1000, so no range at all. To avoid that, force to leave the programming mode
             * temporarily for setting both points again.
             */
            PrintlnSerial_string("Point 2 set", EndPoint_All);
            if (activesettings.pos_start < pos)
            {
                activesettings.pos_end = pos;
            }
            else
            {
                activesettings.pos_end = activesettings.pos_start;
                activesettings.pos_start = pos;
            }
        }
    }
    lastendpointswitch = currentendpointswitch; // Needed to identify a raising flank on the tip switch


    /*
     * Evaluate the Max Acceleration Potentiometer
     */
    uint16_t max_accel = getMaxAccelPoti();
    if (max_accel != 0 && max_accel > activesettings.stick_neutral_pos + activesettings.stick_neutral_range)
    {
        /*
         * (max_accel - neutral) * 10 = 0...700      that would be way too much, should be rather 0..35, so a factor of 20
          */
        activesettings.stick_max_accel = (max_accel - activesettings.stick_neutral_pos - activesettings.stick_neutral_range);
    }

    /*
     * Evaluate the Max Speed Potentiometer
     */
    uint16_t max_speed = getMaxSpeedPoti();
    if (max_speed != 0 && max_speed > activesettings.stick_neutral_pos + activesettings.stick_neutral_range)
    {
       activesettings.stick_max_speed = (max_speed - activesettings.stick_neutral_pos - activesettings.stick_neutral_range);
    }

    /*
     * Evaluate the Mode Switch
     */
    uint16_t modeswitch = getModeSwitch();
    if (modeswitch != 0)
    {
        if (modeswitch < activesettings.stick_neutral_pos - activesettings.stick_neutral_range)
        {
            /*
             * Passthrough
             */
            activesettings.mode = MODE_PASSTHROUGH;
        }
        else if (modeswitch > activesettings.stick_neutral_pos + activesettings.stick_neutral_range)
        {
            /*
             * Full Limiters
             */
            activesettings.mode = MODE_LIMITER_ENDPOINTS;
        }
        else
        {
            activesettings.mode = MODE_LIMITER;
        }

        if (lastmodeswitchsetting != activesettings.mode)
        {
            lastmodeswitchsetting = activesettings.mode;
            PrintlnSerial_string(getCurrentModeLabel(activesettings.mode), EndPoint_All);
        }

    }

    return value;
}

void resetThrottle()
{
    esum = 0.0f;
    ealt = 0.0f;
    yalt = 0.0f;
}

void resetPosTarget()
{
    pos_target = (double) ENCODER_VALUE;
    pos_target_old = pos_target;
}

// ******** Main Loop *********
void controllercycle()
{
    controllerstatus.monitor = FREE; // might be overwritten by stickCycle() so has to come first
    /*
     * speed = change in position per cycle. Speed is always positive.
     * speed = abs(pos_old - pos)
     *
     * time_to_stop = number of cycles it takes to get the current stick value down to neutral with the given activesettings.stick_max_accel
     * time_to_stop = abs(getStick()/max_accel)
     *
     *
     * brake_distance = v�/(2*a) = speed�/(2*a)
     * Problem is, the value of a, the deceleration rate needed is unknown. All we know is the current speed and the stick position.
     * But we know that the current speed should be zero after time_to_stop cycles, hence we know the required
     * deceleration in speed-units: a = speed/time_to_Stop
     *
     * Hence:
     * brake_distance = speed�/(2*a) = speed�/(2*speed/time_to_Stop) = speed * time_to_stop / 2
     *
     * Or in other words: distance = velocity * time; hence brake_distance = velocity * time / 2
     *   |
     * s |--
     * p |  \
     * e |   \
     * e |    \
     * d |     \
     *   |      \
     *   |       \
     *    ----------------------------------
     *      time_to_stop
     */
    int32_t pos_current = ENCODER_VALUE;
    speed_current = abs_d((double) (pos_current_old - pos_current));
    double pos = (double) pos_current;
    double speed_timer = getSpeedPosSensor();
    double speed = speed_current;

    /* If the pos difference is less than 5.0f per 20ms, then the resolution is not high enough. In that
     * case use the time between to pos sensor interrupts to calculate a more precise speed.
     */
    if (speed_current <= 5.0f)
    {
        speed = speed_timer;
    }

    double time_to_stop = abs_d((double) (getStick()/activesettings.stick_max_accel));

    double distance_to_stop = speed * time_to_stop / 2.0f;
    int32_t stick_filtered_value;

    if (activesettings.mode == MODE_ABSOLUTE_POSITION)
    {
        // in case of mode-absolute the actual position does not matter, it is the target position that counts
        stick_filtered_value = stickCycle(pos_target_old, distance_to_stop); // go through the stick position calculation with its limiters, max accel etc
    }
    else
    {
        stick_filtered_value = stickCycle(pos, distance_to_stop); // go through the stick position calculation with its limiters, max accel etc
    }

    /*
     * The stick position and the speed_new variables both define essentially the ESC target. So if the
     * stick is currently set to 100% forward, that means 100% thrust or 100% speed. The main difference is
     * at breaking and here the end point break in particular: In this case the thrust has to be set to zero
     * in order to break as hard as possible whereas the speed output can be gradually reduced to zero.
     */
    int32_t esc_output = stick_filtered_value;

    if (activesettings.mode != MODE_PASSTHROUGH && activesettings.mode != MODE_LIMITER)
    {
        /*
         * In passthrough mode or limiter mode, the cablecam can move freely, no end points are considered. In all other modes...
         */
        if (activesettings.mode == MODE_ABSOLUTE_POSITION)
        {
            /*
             * In absolute mode everything revolves around the target pos. The speed is the change in target pos etc.
             * The actual position does not matter, as the stick controls where the cablecam should be. It is an idealistic world,
             * where the target position can be calculated mathematically correct.
             * The PID loop is responsible to keep the cablecam near the target position.
             */


            /*
             * The new target is the old target increased by the stick signal.
             */
            pos_target += ((double)stick_filtered_value) * activesettings.stick_speed_factor;

            // In OPERATIONAL mode the position including the break distance has to be within the end points, in programming mode you can go past that
            if (controllerstatus.safemode == OPERATIONAL)
            {
                // Of course the target can never exceed the end points, unless in programming mode
                if (pos_target > activesettings.pos_end)
                {
                    pos_target = activesettings.pos_end;
                }
                else if (pos_target < activesettings.pos_start)
                {
                    pos_target = activesettings.pos_start;
                }
            }
            pos_target_old = pos_target;


            /*
             * The PID loop calculates the error between target pos and actual pos and does change the throttle/speed signal in order to keep the error as small as possible.
             */
            double y = 0.0f;


            double e = pos_target - pos;     // This is the amount of steps the target pos does not match the reality
            esum += e;

            if (e >= activesettings.max_position_error || e <= -activesettings.max_position_error)
            {
                // The cablecam cannot catchup with the target position --> EMERGENCY BRAKE
                resetThrottle();
                resetPosTarget();
                esc_output = 0;
                controllerstatus.monitor = EMERGENCYBRAKE;
            }
            else
            {
                // y = Kp*e + Ki*Ta*esum + Kd/Ta*(e � ealt);
                y = (activesettings.P * e) + (activesettings.I * Ta * esum) + (activesettings.D / Ta) * (e - ealt);         // PID loop calculation

                if (activesettings.esc_direction == 1)
                {
                    esc_output = (int32_t) y;
                }
                else
                {
                    esc_output = (int32_t) -y;
                }

                ealt = e;

                if (is1Hz() && abs_d(e) > 1.0f)
                {
                    printPIDMonitor(e, y, EndPoint_USB);
                }
                yalt = y;
            }
        }
        else
        {
            // activesettings.mode != MODE_ABSOLUTE_POSITION

            // esc_out = stick_requested_value for speed based ESCs or in case of a classic ESC esc_out = 0 if monitor == ENDPOINTBREAK
        }
    }

    if (esc_output > 0)
    {
        TIM3->CCR3 = activesettings.esc_neutral_pos + activesettings.esc_neutral_range + (esc_output/activesettings.esc_scale);
    }
    else if (esc_output < 0)
    {
        TIM3->CCR3 = activesettings.esc_neutral_pos - activesettings.esc_neutral_range + (esc_output/activesettings.esc_scale);
    }
    else
    {
        TIM3->CCR3 = activesettings.esc_neutral_pos;
    }
    VESC_Output(esc_output);



    /*
     * Log the last CYCLEMONITOR_SAMPLE_COUNT events in memory.
     * If neither the cablecam moves nor should move (esc_output == 0), then there is nothing interesting to log
     */
    if (speed != 0.0f || esc_output != 0)
    {
        cyclemonitor_t * sample = &controllerstatus.cyclemonitor[controllerstatus.cyclemonitor_position];
        sample->distance_to_stop = distance_to_stop;
        sample->esc = TIM3->CCR3;
        sample->pos = pos;
        sample->speed = speed;
        sample->stick = getStick();
        sample->tick = HAL_GetTick();
        controllerstatus.cyclemonitor_position++;
        if (controllerstatus.cyclemonitor_position > CYCLEMONITOR_SAMPLE_COUNT)
        {
            controllerstatus.cyclemonitor_position = 0;
        }
    }


    pos_current_old = pos_current; // required for the actual speed calculation

    if (is1Hz())
    {
        // printControlLoop(stick_filtered_value, speed, pos, distance_to_stop, controllerstatus.monitor, TIM3->CCR3, EndPoint_USB);
    }
}

void printControlLoop(int16_t input, double speed, double pos, double brakedistance, CONTROLLER_MONITOR_t monitor, uint16_t esc, Endpoints endpoint)
{
    PrintSerial_string("LastValidFrame: ", endpoint);
    PrintSerial_long(sbusdata.sbusLastValidFrame, endpoint);
    PrintSerial_string("  Duty: ", endpoint);
    PrintSerial_int(getDuty(activesettings.rc_channel_speed), endpoint);
    PrintSerial_string("  Raw: ", endpoint);
    PrintSerial_int(sbusdata.servovalues[activesettings.rc_channel_speed].duty, endpoint);
    PrintSerial_string("  ESC Out: ", endpoint);
    PrintSerial_int(esc, endpoint);
    PrintSerial_string("  ", endpoint);
    PrintSerial_string(getSafeModeLabel(), endpoint);
    PrintSerial_string("  ESC Input: ", endpoint);
    PrintSerial_int(input, endpoint);
    PrintSerial_string("  Speed: ", endpoint);
    PrintSerial_double(speed, endpoint);
    PrintSerial_string("  Brakedistance: ", endpoint);
    PrintSerial_double(brakedistance, endpoint);

    PrintSerial_string("  Calculated stop: ", endpoint);
    if (input > 0)
    {
        if (pos + brakedistance > activesettings.pos_end)
        {
            PrintSerial_double(pos + brakedistance, endpoint);
        }
        else
        {
            PrintSerial_string("         ", endpoint);
        }
    }
    else
    {
        if (pos - brakedistance < activesettings.pos_start)
        {
            PrintSerial_double(pos - brakedistance, endpoint);
        }
        else
        {
            PrintSerial_string("         ", endpoint);
        }
    }

    if (monitor == FREE)
    {
        PrintSerial_string("  FREE", endpoint);
    }
    else if (monitor == ENDPOINTBRAKE)
    {
        PrintSerial_string("  ENDPOINTBRAKE", endpoint);
    }
    else if (monitor == EMERGENCYBRAKE)
    {
        PrintSerial_string("  EMERGENCYBRAKE", endpoint);
    }
    else
    {
        PrintSerial_string("  ???", endpoint);
    }

    PrintSerial_string("  Pos: ", endpoint);
    PrintlnSerial_double(pos, endpoint);
}

void printPIDMonitor(double e, double y, Endpoints endpoint)
{
    // y = (activesettings.P * e) + (activesettings.I * Ta * esum) + (activesettings.D / Ta) * (e - ealt);
    PrintSerial_string("y = ", endpoint);
    PrintSerial_double(activesettings.P, endpoint);
    PrintSerial_string("* ", endpoint);
    PrintSerial_double(e, endpoint);
    PrintSerial_string("+   ", endpoint);
    PrintSerial_double(activesettings.I, endpoint);
    PrintSerial_string("* ", endpoint);
    PrintSerial_double(Ta, endpoint);
    PrintSerial_string("* ", endpoint);
    PrintSerial_double(esum, endpoint);
    PrintSerial_string("+   ", endpoint);
    PrintSerial_double(activesettings.D, endpoint);
    PrintSerial_string("* ", endpoint);
    PrintSerial_double(e-ealt, endpoint);
    PrintSerial_string("/ ", endpoint);
    PrintSerial_double(Ta, endpoint);
    PrintSerial_string("= ", endpoint);
    PrintlnSerial_double(y, endpoint);
}
