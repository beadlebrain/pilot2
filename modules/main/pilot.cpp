#include "pilot.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include <Protocol/RFData.h>
#include <Protocol/common.h>
#include <Protocol/usb_data_publish.h>
#include <utils/vector.h>
#include <utils/param.h>
#include <utils/log.h>
#include <utils/console.h>
#include <utils/gauss_newton.h>
#include <protocol/crc32.h>

#include <FileSystem/ff.h>

using namespace HAL;
using namespace devices;
using namespace math;

// constants
#define THROTTLE_STOP ((int)(isnan(pwm_override_min)? max(rc_setting[2][0]-20,1000):pwm_override_min))
#define THROTTLE_MAX ((int)(isnan(pwm_override_max)? min(rc_setting[2][2]-20,2000) : pwm_override_max))
#define SAFE_ON(x) if(x) (x)->on()
#define SAFE_OFF(x) if(x) (x)->off()
#define MAX_MOTOR_COUNT 8
#define SONAR_MIN 0.0f
#define SONAR_MAX 4.5f
const float PI180 = 180/PI;

// parameters
static param forced_mobile_controll("mob", 0);		// use mobile controlling even when RF available
static param use_EKF("ekf", 0);		// use EKF estimator
static param cycle_time("time", 3000);
static param use_alt_estimator2("alt2", 0);		// use new alt estimator
static param crash_protect("prot", 0);		// crash protection
static param pwm_override_max("tmax", NAN);
static param pwm_override_min("tmin", NAN);
static param quadcopter_max_climb_rate("maxC",5);
static param quadcopter_max_descend_rate("maxD", 2);
static param quadcopter_auto_landing_rate_fast("flrt", 1.5f);		// absolute value of fast automated landing speed in meter/s, 
static param quadcopter_auto_landing_rate_final("lrat", 0.5f);		// absolute value of final approach speed in meter/s
static param max_altitude("limV", 100);
static param max_distance("limH", 100);
static param ignore_error("err",0);

static param quadcopter_trim[3] = 
{
	param("trmR", 0 * PI / 18),				// roll
	param("trmP", 0 * PI / 180),			// pitch
	param("trmY", 0.0f),					// yaw
};

static param _gyro_bias[2][4] =	//[p1,p2][temperature,g0,g1,g2]
{
	{param("gbt1", NAN), param("gb11", 0), param("gb21", 0), param("gb31", 0),},
	{param("gbt2", NAN), param("gb12", 0), param("gb22", 0), param("gb32", 0),},
};

static param rc_setting[8][4] = 
{
	{param("rc00", 1000), param("rc01", 1520), param("rc02", 2000), param("rc03", 0),},
	{param("rc10", 1000), param("rc11", 1520), param("rc12", 2000), param("rc13", 0),},
	{param("rc20", 1000), param("rc21", 1520), param("rc22", 2000), param("rc23", 0),},
	{param("rc30", 1000), param("rc31", 1520), param("rc32", 2000), param("rc33", 0),},
	{param("rc40", 1000), param("rc41", 1520), param("rc42", 2000), param("rc43", 0),},
	{param("rc50", 1000), param("rc51", 1520), param("rc52", 2000), param("rc53", 0),},
	{param("rc60", 1000), param("rc61", 1520), param("rc62", 2000), param("rc63", 0),},
	{param("rc70", 1000), param("rc71", 1520), param("rc72", 2000), param("rc73", 0),},
};

static param hall_sensor_sensitivity("hall", 0.0666f);
static param motor_matrix("mat", 0);
static param THROTTLE_IDLE("idle", 1176);
static param acc_bias[3] = 
{
	param("abix", 0), param("abiy", 0), param("abiz", 0),
};
static param acc_scale[3] = 
{
	param("ascx", 1), param("ascy", 1), param("ascz", 1),
};
static param mag_bias[3] = 
{
	param("mbx", 0), param("mby", 0), param("mbz", 0),
};
static param mag_scale[3] = 
{
	param("mgx", 1), param("mgy", 1), param("mgz", 1),
};
static float quadcopter_mixing_matrix[2][MAX_MOTOR_COUNT][3] = // the motor mixing matrix, [motor number] [roll, pitch, yaw]
{
	{							// + mode
		{0, -1, +1},			// rear, CCW
		{-1, 0, -1},			// right, CW
		{0, +1, +1},			// front, CCW
		{+1, 0, -1},			// left, CW
	},
	{							// X mode
		{-0.707f,-0.707f,+0.707f},				//REAR_R, CCW
		{-0.707f,+0.707f,-0.707f},				//FRONT_R, CW
		{+0.707f,+0.707f,+0.707f},				//FRONT_L, CCW
		{+0.707f,-0.707f,-0.707f},				//REAR_L, CW
	}
};

// constructor and destructor, initializer
yet_another_pilot::yet_another_pilot()
:state_led(NULL)
,SD_led(NULL)
,flashlight(NULL)
,rcin(NULL)
,rcout(NULL)
,rgb(NULL)
,range_finder(NULL)
,vcp(NULL)
,mag_calibration_state(0)			// 0: not running, 1: collecting data, 2: calibrating
,last_mag_calibration_result(0xff)	// 0xff: not calibrated at all, other values from mag calibration.
,usb_data_publish(0)
,lowpower(0)		// lowpower == 0:power good, 1:low power, no action taken, 2:low power, action taken.
,acc_cal_requested(false)
,acc_cal_done(false)
,motor_saturated(false)
,rc_fail(0)
,round_running_time(0)
,new_gps_data(false)
,critical_errors(0)
,cycle_counter(0)
,ground_pressure(0)
,ground_temperature(0)
,airborne(false)
,takeoff_ground_altitude(0)
,armed(false)
,flight_mode(basic)
,collision_detected(0)	// remember to clear it before arming
,tilt_us(0)	// remember to clear it before arming
//,gyro_lpf2p({LowPassFilter2p(333.3, 40),LowPassFilter2p(333.3, 40),LowPassFilter2p(333.3, 40)})	// 2nd order low pass filter for gyro.

,last_tick(0)
,last_gps_tick(0)
,voltage(12)
,current(0)
,interval(0)
,new_baro_data(false)
,mah_consumed(0)
,wh_consumed(0)
,sonar_distance(NAN)
,bluetooth_last_update(0)
,mobile_last_update(0)
,avg_count(0)
,loop_hz(0)
,islanding(false)
{
	memset(g_pwm_input_update, 0, sizeof(g_pwm_input_update));
	memset(g_ppm_output, 0, sizeof(g_ppm_output));
	memset(g_pwm_input, 0, sizeof(g_pwm_input));
	memset(acc_avg_count, 0, sizeof(acc_avg_count));
	memset(&imu_statics[0][0], 0, sizeof(imu_statics));
	memset(rc_mobile, 0, sizeof(rc_mobile));
	memset(&gyro_reading, 0, sizeof(gyro_reading));
	memset(&body_rate, 0, sizeof(body_rate));
	memset(&accel, 0, sizeof(accel));
	memset(&v_flow_ned,0,sizeof(v_flow_ned));
	gps_attitude_timeout = 0;
	land_possible = false;
	imu_data_lock = false;
	event_count = 0;
	home_set = false;
	rc_fail_tick = 0;
	pos_control = &pos_control_hybird;

	for(int i=0; i<3; i++)
	{
		gyro_lpf2p[i].set_cutoff_frequency(1000, 60);
		accel_lpf2p[i].set_cutoff_frequency(1000, 60);
	}
}

// helper functions
int yet_another_pilot::send_package(const void *data, uint16_t size, uint8_t type, IUART*uart)
{
	uint8_t start_code[2] = {0x85, 0xa3};
	uart->write(start_code, 2);
	uart->write(&size, 2);
	uart->write(&type, 1);
	uart->write(data, size);

	return size;
}

void yet_another_pilot::output_rc()
{
	rcout->write(g_ppm_output, 0,  min(rcout->get_channel_count(), countof(g_ppm_output)));
}

void yet_another_pilot::STOP_ALL_MOTORS()
{
	for(int i=0; i<16; i++)
		g_ppm_output[i] = THROTTLE_STOP;
	output_rc();
}

int yet_another_pilot::calculate_baro_altitude()
{
	// raw altitude
	double scaling = (double)a_raw_pressure / ground_pressure;
	float temp = ((float)ground_temperature) + 273.15f;
	a_raw_altitude = 153.8462f * temp * (1.0f - exp(0.190259f * log(scaling)));
	if (fabs(a_raw_altitude) < 5.0f)
		ground_temperature = a_raw_temperature;

	return 0;
}

int yet_another_pilot::set_home(const float *new_home)
{
	if (!new_home || !pos_estimator_ready())
		return -1;

	memcpy(home, new_home, sizeof(new_home));
	home_set = true;
	new_event(event_home_set, 0);

	position llh = estimator.NED2LLH(new_home);

	LOGE("home set to:%f / %f, %f - %f\n", home[0], home[1], llh.latitude / double(COORDTIMES), llh.longtitude / double(COORDTIMES));

	return 0;
}

int yet_another_pilot::set_home_LLH(const float * new_home)
{
	if (!estimator.home_set)
		return -1;

	position_meter meter = estimator.LLH2NED(new_home);

	float ne[2] = {meter.latitude, meter.longtitude};

	return set_home(ne);
}

bool yet_another_pilot::pos_estimator_ready()
{
	if (use_EKF > 0.5f)
	{
		// TODO: use EKF estimator to determine healthy
		return estimator.healthy();
	}
	else
	{
		return estimator.healthy();
	}
}

int yet_another_pilot::get_home(float * home_pos)
{
	if (!home_set)
		return -1;

	memcpy(home_pos, home, sizeof(home));
	return 0;
}

int yet_another_pilot::get_pos_velocity_ned(float *pos, float *velocity)
{
	if (!pos_estimator_ready())
		return -1;

	if(use_EKF > 0.5f)
	{
		if (pos)
		{
			pos[0]= ekf_est.ekf_result.Pos_x;
			pos[1]=ekf_est.ekf_result.Pos_y;
		}
		if (velocity)
		{
			velocity[0] = ekf_est.ekf_result.Vel_x;
			velocity[1] = ekf_est.ekf_result.Vel_y;
		}
	}
	else
	{
		position_meter meter = estimator.get_estimation_meter();
		if (pos)
		{
			pos[0]= meter.latitude;
			pos[1]= meter.longtitude;
		}
		if (velocity)
		{
			velocity[0] = meter.vlatitude;
			velocity[1] = meter.vlongtitude;
		}
	}

	return 0;
}

int yet_another_pilot::default_alt_controlling()
{
	bool fast_stage = (alt_estimator.state[0] > takeoff_ground_altitude + 10.0f) && !alt_controller.sonar_actived();
	float landing_rate = fast_stage ? quadcopter_auto_landing_rate_fast : quadcopter_auto_landing_rate_final;
	float max_climb_rate = islanding ? (landing_rate + quadcopter_auto_landing_rate_final) : quadcopter_max_climb_rate;	// very low climbe rate even if max throttle in landing state
	
	float v = rc[2] - 0.5f;
	float user_rate;
	if (fabs(v)<0.05f)
		user_rate = 0;
	else if (v>= 0.05f)
	{
		user_rate = (v-0.05f)/0.45f;
		user_rate = user_rate * user_rate * max_climb_rate;
	}
	else
	{
		user_rate = (v+0.05f)/0.45f;
		user_rate = -user_rate * user_rate * quadcopter_max_descend_rate;
	}
	
	// landing?
	if(islanding)
	{
		if (fast_stage)
			user_rate = -landing_rate;		// disable user interaction in fast landing stage.
		else
			user_rate -= landing_rate;		// landing rate is combination of user stick and automated landing rate.
	}

	// altitude limit
	bool vertical_limit_reached = isnan(max_altitude) ? false : (alt_estimator.state[0] > takeoff_ground_altitude + max_altitude);
	if (vertical_limit_reached)
		user_rate = fmin(user_rate, 0);

	alt_controller.update(interval, user_rate);
	
	throttle_result = alt_controller.get_result();

	TRACE("\rthr=%f/%f", throttle_result, alt_controller.get_result());

	return 0;
}

int yet_another_pilot::run_controllers()
{
	attitude_controll.provide_states(euler, use_EKF > 0.5f ? &ekf_est.ekf_result.q0 : &q0, body_rate.array, motor_saturated ? LIMIT_ALL : LIMIT_NONE, airborne);

	if (use_alt_estimator2 > 0.5f)
	{	
		float alt_state[3] = {alt_estimator2.x[0], alt_estimator2.x[1], alt_estimator2.x[2] + acc_ned[2]};
		alt_controller.provide_states(alt_state, sonar_distance, euler, throttle_real, LIMIT_NONE, airborne);
	}
	else
	{
		float alt_state[3] = {alt_estimator.state[0], alt_estimator.state[1], alt_estimator.state[3] + acc_ned[2]};			
		alt_controller.provide_states(alt_state, sonar_distance, euler, throttle_real, LIMIT_NONE, airborne);
	}

	switch(flight_mode)
	{
		case basic:
			mode_basic.loop(interval);
			break;
		case althold:
			mode_althold.loop(interval);
			break;
		case poshold:
			mode_poshold.loop(interval);
			break;
		case optical_flow:
			mode_of_loiter.loop(interval);
			break;
		case RTL:
			mode_RTL.loop(interval);
			break;
	}
	
	attitude_controll.update(interval);


	// check airborne
	if ((alt_estimator.state[0] > takeoff_ground_altitude + 1.0f) ||
		(alt_estimator.state[0] > takeoff_ground_altitude && throttle_result > alt_controller.throttle_hover) ||
		(throttle_result > alt_controller.throttle_hover + QUADCOPTER_THROTTLE_RESERVE))
	{
		if (!airborne && armed)
		{
			airborne = true;
			new_event(event_airborne, 0);
		}
	}
	
	return 0;
}

float min_throttle;
float max_throttle;

int yet_another_pilot::output()
{
	float pid_result[3];
	motor_saturated = false;
	attitude_controll.get_result(pid_result);
	if (armed)
	{
		int matrix = (float)motor_matrix;
		float motor_output[MAX_MOTOR_COUNT] = {0};

		
		// how many motor exists in this motor matrix?
		static int motor_count = 0;
		if(0==motor_count)
		for(int i=0; i<MAX_MOTOR_COUNT; i++)
		{
			if (quadcopter_mixing_matrix[matrix][i][0] == quadcopter_mixing_matrix[matrix][i][1] && 
				quadcopter_mixing_matrix[matrix][i][1] == quadcopter_mixing_matrix[matrix][i][2] && 
				fabs((float)quadcopter_mixing_matrix[matrix][i][2]) < 0.01f)
			{
				motor_count = i;
				break;
			}
		}

		// find how much motor power roll and pitch command requires
		float min_roll_pitch = 1.0f;
		float max_roll_pitch = 0.0f;
		for(int i=0; i<motor_count; i++)
		{
			motor_output[i] = 0;
			for(int j=0; j<2; j++)
				motor_output[i] += quadcopter_mixing_matrix[matrix][i][j] * pid_result[j] * QUADCOPTER_THROTTLE_RESERVE;
			
			min_roll_pitch = fmin(min_roll_pitch, motor_output[i]);
			max_roll_pitch = fmax(max_roll_pitch, motor_output[i]);
		}
		
		// handle saturation caused by roll and pitch alone
		float roll_pitch_factor = 1.0f;
		min_roll_pitch = 1.0f;
		max_roll_pitch = 0.0f;
		if (max_roll_pitch - min_roll_pitch > 1.0f)
			roll_pitch_factor = limit(roll_pitch_factor, 0.5f, 1.0f/(max_roll_pitch - min_roll_pitch));
		for(int i=0; i<motor_count; i++)
		{
			motor_output[i] = 0;
			for(int j=0; j<2; j++)
				motor_output[i] += quadcopter_mixing_matrix[matrix][i][j] * (roll_pitch_factor * pid_result[j]) * QUADCOPTER_THROTTLE_RESERVE;
			
			min_roll_pitch = fmin(min_roll_pitch, motor_output[i]);
			max_roll_pitch = fmax(max_roll_pitch, motor_output[i]);
		}
		
		// add throttle 
		min_throttle = limit(-min_roll_pitch, 0, 1);
		max_throttle = limit(1-max_roll_pitch, 0, 1);
		float throttle = !airborne ? throttle_result : limit(throttle_result, min_throttle, max_throttle);	// add throttle only if airborne
		for(int i=0; i<motor_count; i++)
			motor_output[i] += throttle;
		
		// try ouput max possible yaw command
		float yaw_factor = 1.0f;
		for(int i=0; i<motor_count; i++)
		{
			float yaw_power = quadcopter_mixing_matrix[matrix][i][2] * pid_result[2] * QUADCOPTER_THROTTLE_RESERVE;
			
			if (yaw_power < 0)
			{
				yaw_factor = fmin(yaw_factor, fmax(0, motor_output[i]) / -yaw_power);
			}
			else
			{
				yaw_factor = fmin(yaw_factor, fmax(0, 1-motor_output[i]) / yaw_power);
			}	
			
		}
		yaw_factor = 1.0f;
		//printf("\rf=%.3f(%.2f,%.2f,%.2f,%.2f, r=%.2f, t=%.2f)       ", yaw_factor, motor_output[0], motor_output[1], motor_output[2], motor_output[3], pid_result[2], throttle);
		for(int i=0; i<motor_count; i++)
		{
			motor_output[i] += quadcopter_mixing_matrix[matrix][i][2] * yaw_factor * pid_result[2] * QUADCOPTER_THROTTLE_RESERVE;
			g_ppm_output[i] = limit(THROTTLE_IDLE + motor_output[i]*(THROTTLE_MAX-THROTTLE_IDLE), THROTTLE_IDLE, THROTTLE_MAX);
		}
		
		// statics
		throttle_real = 0;
		for(int i=0; i<motor_count; i++)
			throttle_real += motor_output[i];
		throttle_real /= motor_count;
		
		// placebo motor saturation detector.
		for(int i=0; i<motor_count; i++)
		{
			if (g_ppm_output[i] <= THROTTLE_IDLE+20 || g_ppm_output[i] >= THROTTLE_MAX-20)
				motor_saturated = true;
		}

	}

	else
	{
		STOP_ALL_MOTORS();
	}

	output_rc();
	return 0;
}



// called by main loop, only copy logs to a memory buffer, should be very fast
int yet_another_pilot::save_logs()
{
	if (!storage_ready)
		return 0;

	// send/store debug data
	int64_t systime = systimer->gettime();
	sensor_data sensor =
	{
		{mag_uncalibrated.array[0] * 10, mag_uncalibrated.array[1] * 10, mag_uncalibrated.array[2] * 10},
		{accel_uncalibrated.array[0] * 100, accel_uncalibrated.array[1] * 100, accel_uncalibrated.array[2] * 100},
		mpu6050_temperature * 100 - 10000,
		{gyro_uncalibrated.array[0] * 18000/PI, gyro_uncalibrated.array[1] * 18000/PI, gyro_uncalibrated.array[2] * 18000/PI},
		voltage * 1000,
		current * 1000,
	};
	log(&sensor, TAG_SENSOR_DATA, systime);

	imu_data imu = 
	{
		a_raw_pressure,
		a_raw_temperature,
	
		{body_rate.array[0] * 18000/PI, body_rate.array[1] * 18000/PI, body_rate.array[2] * 18000/PI},
		{accel.array[0] * 100, accel.array[1] * 100, accel.array[2] * 100},
		{mag.array[0] * 10, mag.array[1] * 10, mag.array[2] * 10},
	};
	log(&imu, TAG_IMU_DATA, systime);
	log(&frame, TAG_PX4FLOW_DATA, systime);
	
	position p = estimator.get_estimation();
	position_meter pmeter = estimator.get_estimation_meter();
	ned_data ned = 
	{
		0,
		{acc_ned[0] * 1000, acc_ned[1] * 1000, acc_ned[2] * 1000},
		p.latitude * double(10000000.0/COORDTIMES), 
		p.longtitude * double(10000000.0/COORDTIMES), 
		error_lat : pmeter.vlatitude*100,
		error_lon : pmeter.vlongtitude*100,
	};

	log(&ned, TAG_NED_DATA, systime);


	pilot_data pilot = 
	{
		alt_estimator.state[0] * 100,
		0,//airspeed_sensor_data * 1000,
		{attitude_controll.pid[0][0]*180*100/PI, attitude_controll.pid[1][0]*180*100/PI, attitude_controll.pid[2][0]*180*100/PI},
		{attitude_controll.body_rate_sp[0]*180*100/PI, attitude_controll.body_rate_sp[1]*180*100/PI, attitude_controll.body_rate_sp[2]*180*100/PI},
		armed ? 1 : 0,
		mah_consumed,
	};

	log(&pilot, TAG_PILOT_DATA, systime);

	pilot_data2 pilot2 = 
	{
		{attitude_controll.pid[0][1]*180*100/PI, attitude_controll.pid[1][1]*180*100/PI, attitude_controll.pid[2][1]*180*100/PI},
		{attitude_controll.pid[0][2]*180*100/PI, attitude_controll.pid[1][2]*180*100/PI, attitude_controll.pid[2][2]*180*100/PI},
	};

	log(&pilot2, TAG_PILOT_DATA2, systime);

	ppm_data ppm = 
	{
		{g_pwm_input[0], g_pwm_input[1], g_pwm_input[2], g_pwm_input[3], g_pwm_input[4], g_pwm_input[5]},
		{g_ppm_output[0], g_ppm_output[1], g_ppm_output[2], g_ppm_output[3], g_ppm_output[4], g_ppm_output[5]},
	};

	log(&ppm, TAG_PPM_DATA, systime);
	
	ekf_data ekf =
	{
		{ekf_est.ekf_result.roll* 18000/PI,ekf_est.ekf_result.pitch* 18000/PI,ekf_est.ekf_result.yaw* 18000/PI},
		estimator.get_raw_meter().latitude*100,estimator.get_raw_meter().longtitude*100,
		ground_speed_north*1000,ground_speed_east*1000,
		v_flow_ned[0]*1000,v_flow_ned[1]*1000,//optical flow ned velocity
		{ekf_est.ekf_result.Pos_x*100,ekf_est.ekf_result.Pos_y*100,ekf_est.ekf_result.Pos_z*100},
		{ekf_est.ekf_result.Vel_x*1000,ekf_est.ekf_result.Vel_y*1000,ekf_est.ekf_result.Vel_z*1000},
	};
	
	log2(&ekf, TAG_EKF_DATA,sizeof(ekf));
	
	quadcopter_data quad = 
	{
		euler[0] * 18000/PI, euler[1] * 18000/PI, euler[2] * 18000/PI,
		attitude_controll.euler_sp[0] * 18000/PI, attitude_controll.euler_sp[1] * 18000/PI, attitude_controll.euler_sp[2] * 18000/PI,
		body_rate.array[0] * 18000/PI, body_rate.array[1] * 18000/PI, body_rate.array[2] * 18000/PI,
		attitude_controll.body_rate_sp[0] * 18000/PI,  attitude_controll.body_rate_sp[1] * 18000/PI, attitude_controll.body_rate_sp[2] * 18000/PI, 
	};

	log(&quad, TAG_QUADCOPTER_DATA, systime);


	quadcopter_data2 quad2 = 
	{
		alt_estimator.state[1] * 100,
		airborne,
		flight_mode,
		alt_estimator.state[0] * 100,
		alt_estimator.state[2] * 100,
		a_raw_altitude * 100,
		0,//acc_ned[2]_mwc * 100,
		loop_hz,
		THROTTLE_IDLE + throttle_result * (THROTTLE_MAX-THROTTLE_IDLE),
		kalman_accel_bias : alt_estimator.state[3] * 1000,
		{gyro_bias[0] * 1800000/PI, gyro_bias[1] * 1800000/PI, gyro_bias[2] * 1800000/PI,}
	};

	log(&quad2, TAG_QUADCOPTER_DATA2, systime);

	float mag_size = sqrt(mag.array[0]*mag.array[0]+mag.array[1]*mag.array[1]+mag.array[2]*mag.array[2]);
	float erra = err_a[0] * err_a[0] + err_a[1] * err_a[1] + err_a[2] * err_a[2];
	float errm = err_m[0] * err_m[0] + err_m[1] * err_m[1] + err_m[2] * err_m[2];
	quadcopter_data4 quad4 = 
	{
		isnan(alt_controller.m_sonar_target) ? 0 : alt_controller.m_sonar_target*100,
		mag_size,
		ground_accel_north*2000,
		ground_accel_east*2000,
		climb_rate_gps*1000,
		raw_yaw * 18000 / PI,
		acc_horizontal[0] * 1000,
		acc_horizontal[1] * 1000,
		{erra * 18000/PI, errm * 18000/PI, err_a[2] * 18000/PI},
	};
	log(&quad4, TAG_QUADCOPTER_DATA4, systime);

	extern uint32_t lost1;
	extern uint32_t lost2;
	quadcopter_data5 quad5 = 
	{
		{q0,q1,q2,q3},
		ground_speed_north*100,
		ground_speed_east*100,
		round_running_time,
		motor_saturated,
	};
	log(&quad5, TAG_QUADCOPTER_DATA5, systime);

	quadcopter_data3 quad3 = 
	{
		alt_controller.baro_target * 100,
		0,//alt_estimatorCF.state[0] * 100,
		alt_controller.target_climb_rate * 100,
		0,//alt_estimatorCF.state[1] * 100,
		alt_controller.target_accel * 100,
		0,//(acc_ned[2] + alt_estimatorCF.state[3]) * 100,
		throttle_result*1000,
		yaw_launch * 18000 / PI,
		euler[2] * 18000 / PI,
		alt_controller.throttle_hover*1000,
		0,//sonar_result(),
		alt_controller.accel_error_pid[1]*1000,
	};

	log(&quad3, TAG_QUADCOPTER_DATA3, systime);

	// pos controller data1
	pos_controller_data pc = 
	{
		pos_control_hybird.setpoint[0],
		pos_control_hybird.setpoint[1],
		pos_control_hybird.pos[0],
		pos_control_hybird.pos[1],
		pos_control_hybird.target_velocity[0]*1000,
		pos_control_hybird.target_velocity[1]*1000,
		pos_control_hybird.velocity[0]*1000,
		pos_control_hybird.velocity[1]*1000,
	};
	log(&pc, TAG_POS_CONTROLLER_DATA1, systime);


	// pos controller data2
	pos_controller_data2 pc2 = 
	{
		pos_control_hybird.target_accel[0]*1000,
		pos_control_hybird.target_accel[1]*1000,
		{
			{pos_control_hybird.pid[0][0]*100, pos_control_hybird.pid[0][1]*100, pos_control_hybird.pid[0][2]*100,},
			{pos_control_hybird.pid[1][0]*100, pos_control_hybird.pid[1][1]*100, pos_control_hybird.pid[1][2]*100,},
		}
	};

	log(&pc2, TAG_POS_CONTROLLER_DATA2, systime);

	if (last_gps_tick > systimer->gettime() - 2000000)
	{
		static unsigned short gps_id = 0;
		::gps_data data = 
		{
			{gps.DOP[0], gps.DOP[1], gps.DOP[2]},
			gps.speed*100,
			gps.longitude * 10000000, gps.latitude * 10000000, gps.altitude,
			gps.satelite_in_view, gps.satelite_in_use,
			gps.sig, gps.fix,
			gps_id++ & 0xf,
			gps.direction,
		};

		log(&data, TAG_GPS_DATA, systime);
	}

	rc_mobile_data mobile = 
	{
		{rc_mobile[0] * 1000, rc_mobile[1] * 1000, rc_mobile[2] * 1000, rc_mobile[3] * 1000,},
		min((systimer->gettime() - mobile_last_update)/1000, 65535),
	};
	log(&mobile, TAG_MOBILE_DATA, systime);
	
	return 0;
}

int yet_another_pilot::read_sensors()
{	
	if (range_finder)
	{
		range_finder->trigger();
		float distance = 0;
		if (0 == range_finder->read(&distance))
		{
			if (distance <= SONAR_MIN || distance >= SONAR_MAX)
				sonar_distance = NAN;
			else
				sonar_distance = distance;
		}
		
		if (isnan(sonar_distance))
			printf("\rNAN     ");
		else
			printf("\r%.3f", sonar_distance);
	}
	

	// read GPSs
	int lowest_hdop = 100000;
	new_gps_data = false;
	for(int i=0; i<manager.get_GPS_count(); i++)
	{
		IGPS *gps = manager.get_GPS(i);
		if (!gps->healthy())
			continue;

		devices::gps_data data;
		int res = gps->read(&data);

		// TODO: select best GPS correctly
		if (res == 0 && data.DOP[1] > 0 && data.DOP[1] < lowest_hdop)
		{
			lowest_hdop = data.DOP[1];
			this->gps = data;
			new_gps_data = (res == 0);
			last_gps_tick = systimer->gettime();

			log2(&data, TAG_EXTRA_GPS_DATA, sizeof(data));
		}
	}
	if (manager.get_GPS_count() == 0)
		critical_errors |= error_GPS;

	if (vcp && (usb_data_publish & data_publish_gps) && new_gps_data)
	{
		if (usb_data_publish & data_publish_binary)
		{
			usb_gps_data data = 
			{
				systimer->gettime(),
				gps.latitude,
				gps.longitude,
				gps.altitude*100,
				gps.DOP[1],
				gps.speed,
				gps.direction,
			};

			send_package(&data, sizeof(data), data_publish_gps, vcp);
		}
		else
		{
			char tmp[200];
			sprintf(tmp, "gps:%.3f,%.6f,%.6f,%.2f,%.2f,%.2f,%.0f\n", systimer->gettime()/1000000.0f,
			gps.latitude, gps.longitude, gps.altitude, gps.DOP[1]/100.0f, gps.speed, gps.direction);
			vcp->write(tmp, strlen(tmp));
		}
	}
	
		// accelerometer motion detector and calibration.
	motion_acc.new_data(accel_uncalibrated);
	if (motion_acc.get_average(NULL) > 100)
	{
		vector avg;
		int avg_counter = motion_acc.get_average(&avg);
		int i = -1;
		if (avg.array[0] < -8.5f)			// 3 times of datasheet "typical" zero G output, +-150mg for XY, +-280mg for Z.
			i = 0;
		else if (avg.array[0] > 8.5f)
			i = 1;
		else if (avg.array[1] < -8.5f)
			i = 2;
		else if (avg.array[1] > 8.5f)
			i = 3;
		else if (avg.array[2] < -7.2f)
			i = 4;
		else if (avg.array[2] > 7.2f)
			i = 5;

		if (i>=0 && i<6)
		{
			if (avg_counter > acc_avg_count[i])
			{
				acc_avg_count[i] = avg_counter;
				acc_calibrator[i] = avg;
			}
		}
	}
	finish_accel_cal();

	// statics
	for(int i=0; i<3; i++)
	{
		imu_statics[0][0].array[i] = fmin(accel.array[i], imu_statics[0][0].array[i]);
		imu_statics[0][1].array[i] = accel.array[i];
		imu_statics[0][2].array[i] = fmax(accel.array[i], imu_statics[0][2].array[i]);
		imu_statics[0][3].array[i] = accel.array[i] + imu_statics[0][3].array[i];

		imu_statics[1][0].array[i] = fmin(gyro_uncalibrated.array[i], imu_statics[1][0].array[i]);
		imu_statics[1][1].array[i] = gyro_uncalibrated.array[i];
		imu_statics[1][2].array[i] = fmax(gyro_uncalibrated.array[i], imu_statics[1][2].array[i]);
		imu_statics[1][3].array[i] = gyro_uncalibrated.array[i] + imu_statics[1][3].array[i];
	}
	avg_count ++;

	return 0;
}

int yet_another_pilot::read_imu_and_filter()
{
	int64_t reading_start = systimer->gettime();
	vector acc = {0};
	vector gyro = {0};
	vector mag = {0};

	// read gyros
	int healthy_gyro_count = 0;
	for(int i=0; i<min(manager.get_gyroscope_count(),1); i++)
	{
		IGyro* gyroscope = manager.get_gyroscope(i);
		
		gyro_data data;
		if (!gyroscope->healthy() || gyroscope->read(&data) < 0)
			continue;
		
		mpu6050_temperature = data.temperature;
		gyro.V.x += data.x;
		gyro.V.y += data.y;
		gyro.V.z += data.z;

		healthy_gyro_count ++;
	}
	if (healthy_gyro_count == 0)
		critical_errors |= error_gyro;
	else
	{
		gyro.V.x /= healthy_gyro_count;
		gyro.V.y /= healthy_gyro_count;
		gyro.V.z /= healthy_gyro_count;
	}

	// read accelerometers
	int healthy_acc_count = 0;
	for(int i=0; i<min(manager.get_accelerometer_count(),1); i++)
	{
		IAccelerometer* accelerometer = manager.get_accelerometer(i);
		
		accelerometer_data data;
		if (!accelerometer->healthy() || accelerometer->read(&data) < 0)
			continue;

		acc.V.x += data.x;
		acc.V.y += data.y;
		acc.V.z += data.z;

		healthy_acc_count ++;
	}
	if (healthy_acc_count == 0)
		critical_errors |= error_accelerometer;
	else
	{
		acc.V.x /= healthy_acc_count;
		acc.V.y /= healthy_acc_count;
		acc.V.z /= healthy_acc_count;
	}
	
	// read magnetometer and barometer since we don't have lock support and it is connected to same SPI bus with gyro and acceleromter
	// at lower rate (50hz)
	static int64_t last_mag_read = 0;
	bool got_mag = false;
	if (!imu_data_lock && systimer->gettime() - last_mag_read > 20000)
	{
		last_mag_read = systimer->gettime();
		got_mag = true;

		// read magnetometers
		int healthy_mag_count = 0;
		for(int i=0; i<manager.get_magnetometer_count(); i++)
		{
			IMagnetometer* magnetometer = manager.get_magnetometer(i);
			
			mag_data data;
			if (!magnetometer->healthy() || magnetometer->read(&data) < 0)
				continue;
			
			mag.V.x += data.x;
			mag.V.y += data.y;
			mag.V.z += data.z;

			healthy_mag_count ++ ;
		}
		if (healthy_mag_count == 0)
			critical_errors |= error_magnet;
		else
		{
			mag.V.x /= healthy_mag_count;
			mag.V.y /= healthy_mag_count;
			mag.V.z /= healthy_mag_count;
		}

		if (vcp && (usb_data_publish & data_publish_imu))
		{
			gyro_data gyro2 = {0};
			accelerometer_data acc2 = {0};

			if (manager.get_accelerometer_count()>=2)
				manager.get_accelerometer(1)->read(&acc2);
			if (manager.get_gyroscope_count()>=2)
				manager.get_gyroscope(1)->read(&gyro2);

			if (usb_data_publish & data_publish_binary)
			{
				usb_imu_data data = 
				{
					systimer->gettime(),
					{accel_uncalibrated.V.x * 1000, accel_uncalibrated.V.y * 1000, accel_uncalibrated.V.z * 1000,},
					{gyro_uncalibrated.V.x * 18000 / PI, gyro_uncalibrated.V.y * 18000 / PI, gyro_uncalibrated.V.z * 18000 / PI,},
					{acc2.x * 1000, acc2.y * 1000, acc2.z * 1000,},
					{gyro2.x * 18000 / PI, gyro2.y * 18000 / PI, gyro2.z * 18000 / PI,},
					{mag.V.x, mag.V.y, mag.V.z,},
				};

				send_package(&data, sizeof(data), data_publish_imu, vcp);
			}
			else
			{
				char tmp[200];
				sprintf(tmp, 
					"imu:%.4f"
					",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f"
					",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", systimer->gettime()/1000000.0f,
				accel_uncalibrated.V.x, accel_uncalibrated.V.y, accel_uncalibrated.V.z, gyro_uncalibrated.V.x, gyro_uncalibrated.V.y, gyro_uncalibrated.V.z, mag.V.x, mag.V.y, mag.V.z, mpu6050_temperature,
				acc2.x, acc2.y, acc2.z, gyro2.x, gyro2.y, gyro2.z);
				
				vcp->write(tmp, strlen(tmp));
			}
		}


		// read barometers
		int healthy_baro_count = 0;
		for(int i=0; i<manager.get_barometer_count(); i++)
		{
			IBarometer* barometer = manager.get_barometer(i);
			
			baro_data data;
			if (!barometer->healthy())
				continue;
			int res = barometer->read(&data);
			if (res < 0)
				continue;

			new_baro_data = res == 0;
			a_raw_temperature = data.temperature;
			a_raw_pressure = data.pressure;

			healthy_baro_count ++;
		}
		if (healthy_baro_count == 0)
			critical_errors |= error_baro;
		
		if (vcp && (usb_data_publish & data_publish_baro) && new_baro_data)
		{
			if (usb_data_publish & data_publish_binary)
			{
				usb_baro_data data =
				{
					systimer->gettime(),
					a_raw_pressure,
					a_raw_temperature*100
				};

				send_package(&data, sizeof(data), data_publish_baro, vcp);
			}
			else
			{
				char tmp[200];
				sprintf(tmp, "baro:%.3f,%.0f,%.3f\n", systimer->gettime()/1000000.0f, a_raw_pressure, a_raw_temperature);
				vcp->write(tmp, strlen(tmp));
			}
		}
	}
	
	// bias and scale calibrating
	float temperature_delta = mpu6050_temperature - temperature0;
	float gyro_bias[3] = 
	{
		-(temperature_delta * gyro_temp_k.array[0] + gyro_temp_a.array[0]),
		-(temperature_delta * gyro_temp_k.array[1] + gyro_temp_a.array[1]),
		-(temperature_delta * gyro_temp_k.array[2] + gyro_temp_a.array[2]),
	};
	accel_uncalibrated = acc;
	gyro_uncalibrated = gyro_reading;
	if (got_mag)
		mag_uncalibrated = mag;
	
	for(int i=0; i<3; i++)
	{
		acc.array[i] += acc_bias[i];
		acc.array[i] *= acc_scale[i];
		mag.array[i] += mag_bias[i];
		mag.array[i] *= mag_scale[i];
		gyro_reading.array[i] += gyro_bias[i];
	}
	
	// copy mag
	if (!imu_data_lock && got_mag)
	{
		this->mag = mag;
	}

	// apply a 40hz 2nd order LPF to accelerometer readings
	// stop overwriting target data if imu data lock acquired
	if (!imu_data_lock)
	{
		float alpha = 0.001f / (0.001f + 1.0f/(2*PI * 20.0f));
		for(int i=0; i<3; i++)
//			this->accel.array[i] = accel_lpf2p[i].apply(acc.array[i]);
 			this->accel.array[i] = acc.array[i]*alpha + this->accel.array[i] * (1-alpha);
	}
	else
	{
		for(int i=0; i<3; i++)
			accel_lpf2p[i].apply(acc.array[i]);
	}


	// read optical flow @ 100hz
	static int64_t last_flow_reading = 0;
	if (manager.get_flow_count() && systimer->gettime() - last_flow_reading > 10000)
	{
		last_flow_reading = systimer->gettime();

		sensors::IFlow *flow = manager.get_flow(0);

		if (flow->read_flow(&frame) < 0)
			sonar_distance = NAN;
		else
		{
			static int last_frame_count = 0;
			if (last_frame_count != frame.frame_count && vcp && (usb_data_publish & data_publish_flow))
			{
				last_frame_count = frame.frame_count;

				if (usb_data_publish & data_publish_binary)
				{
					usb_flow_data data = 
					{
						systimer->gettime(),
						frame.pixel_flow_x_sum,
						frame.pixel_flow_y_sum,
						frame.ground_distance,
					};

					send_package(&data, sizeof(data), data_publish_flow, vcp);
				}
				else
				{
					char tmp[100];
					sprintf(tmp, "flow:%.3f,%d,%d,%f\n", systimer->gettime()/1000000.0f, frame.pixel_flow_x_sum, frame.pixel_flow_y_sum, frame.ground_distance/1000.0f);
					vcp->write(tmp, strlen(tmp));
				}
			}

			sonar_distance = frame.ground_distance / 1000.0f;
			if (sonar_distance <= SONAR_MIN || sonar_distance >= SONAR_MAX)
				sonar_distance = NAN;

			//float flowx = frame.pixel_flow_x_sum - body_rate.array[0] * 18000/PI * 0.006f;
			//float floay = frame.pixel_flow_y_sum - body_rate.array[1] * 18000/PI * 0.006f;

			//frame.gyro_x_rate = flowx;
			//frame.gyro_y_rate = floay;
		}
	}
	
	
	// apply a 2nd order LPF to gyro readings
	// stop overwriting target data if imu data lock acquired
	if (!imu_data_lock)
	{
		for(int i=0; i<3; i++)
			this->gyro_reading.array[i] = gyro_lpf2p[i].apply(gyro.array[i]);
	}
	else
	{
		for(int i=0; i<3; i++)
			gyro_lpf2p[i].apply(gyro.array[i]);
	}

	// log unfiltered imu data
	int16_t data[6] = {acc.V.x * 1000, acc.V.y * 1000, acc.V.z * 1000,
						gyro.V.x * 18000 / PI, gyro.V.y * 18000 / PI, gyro.V.z * 18000 / PI,};

	//log2(data, 5, sizeof(data));

	// voltage and current sensors	
	float alpha = interval / (interval + 1.0f/(2*PI * 2.0f));		// 2hz low pass filter
	if (manager.getBatteryVoltage("BatteryVoltage"))
		voltage = voltage * (1-alpha) + alpha * manager.getBatteryVoltage("BatteryVoltage")->read();
	else
		voltage = NAN;
	if (manager.getBatteryVoltage("BatteryCurrent"))
		current = current * (1-alpha) + alpha * manager.getBatteryVoltage("BatteryCurrent")->read();	
	else
		current = NAN;

	mah_consumed += fabs(current) * interval / 3.6f;	// 3.6 mah = 1As



	return 0;
}

int yet_another_pilot::calculate_state()
{
	if (interval <=0 || interval > 0.2f)
		return -1;

	float factor = 1.0f;
	float factor_mag = 1.0f;

	float acc_gps_bf[3] = {0};

	if (gps_attitude_timeout > 5)
	{
		float gps_acc_ef[3] = {ground_accel_north, ground_accel_east, 0};
		for (int i = 0; i < 3; i++)
		{
			acc_gps_bf[i] = 0.0f;

			for (int j = 0; j < 3; j++)
				acc_gps_bf[i] += NED2BODY[i][j] * gps_acc_ef[j];
		}
	}

	NonlinearSO3AHRSupdate(
	-accel.array[0], -accel.array[1], -accel.array[2], 
	-mag.array[0], -mag.array[1], mag.array[2],
	gyro_reading.array[0], gyro_reading.array[1], gyro_reading.array[2],
	0.15f*factor, 0.0015f, 0.15f*factor_mag, 0.0015f, interval,
	acc_gps_bf[0], acc_gps_bf[1], acc_gps_bf[2]);
//	
	//EKF estimator update
	EKF_U ekf_u;
	EKF_Mesurement ekf_mesurement;
	
	ekf_u.accel_x=accel.array[0];
	ekf_u.accel_y=accel.array[1];
	ekf_u.accel_z=accel.array[2];
	ekf_u.gyro_x=gyro_reading.array[0];
	ekf_u.gyro_y=gyro_reading.array[1];
	ekf_u.gyro_z=gyro_reading.array[2];
	ekf_mesurement.Mag_x=mag.array[0];
	ekf_mesurement.Mag_y=mag.array[1];
	ekf_mesurement.Mag_z=mag.array[2];

	ekf_mesurement.Pos_Baro_z=a_raw_altitude;
	if(estimator.healthy())
	{
		ekf_mesurement.Pos_GPS_x=estimator.get_raw_meter().latitude;
		ekf_mesurement.Pos_GPS_y=estimator.get_raw_meter().longtitude;
		ekf_mesurement.Vel_GPS_x=ground_speed_north;
		ekf_mesurement.Vel_GPS_y=ground_speed_east;
//		ekf_est.set_mesurement_R(1.0E-3,0.08);
		ekf_est.set_mesurement_R(0.0005f*gps.position_accuracy_horizontal * gps.position_accuracy_horizontal, 0.02f*gps.velocity_accuracy_horizontal * gps.velocity_accuracy_horizontal);
	}
	else
	{	
		float pixel_compensated_x = frame.pixel_flow_x_sum - body_rate.array[0] * 18000 / PI * 0.0028f;
		float pixel_compensated_y = frame.pixel_flow_y_sum - body_rate.array[1] * 18000 / PI * 0.0028f;

		float wx = pixel_compensated_x / 28.0f * 100 * PI / 180;
		float wy = pixel_compensated_y / 28.0f * 100 * PI / 180;
		
		float v_flow_body[3];
		v_flow_body[0] = wy * frame.ground_distance/1000.0f * 1.15f;//black magic
		v_flow_body[1] = -wx * frame.ground_distance/1000.0f * 1.15f;
		v_flow_body[2] = 0;

		if (yap.frame.qual < 100)
		{
			v_flow_body[0] = v_flow_body[1] = 0;
			ekf_est.set_mesurement_R(1E20,1E-2);
		}
		else
		{
			ekf_est.set_mesurement_R(1E20, 1E-2);
		}

		
		ekf_est.tf_body2ned(v_flow_body,v_flow_ned);
		
		ekf_mesurement.Pos_GPS_x=0;
		ekf_mesurement.Pos_GPS_y=0;
		ekf_mesurement.Vel_GPS_x=v_flow_ned[0];
		ekf_mesurement.Vel_GPS_y=v_flow_ned[1];
	}
	
	int64_t t = systimer->gettime();
	if (use_EKF > 0.5f)
		ekf_est.update(ekf_u,ekf_mesurement,interval);
	t = systimer->gettime() - t;
// 	printf("%f,%d\r\n",interval, int(t));

	//For debug
	float ekf_buffer[6];
	ekf_buffer[0]=ekf_est.ekf_result.roll*180/3.1415f;
	ekf_buffer[1]=ekf_est.ekf_result.pitch*180/3.1415f;
	ekf_buffer[2]=ekf_est.ekf_result.yaw*180/3.1415f;
	ekf_buffer[3]=euler[0]*180/3.1415f;
	ekf_buffer[4]=euler[1]*180/3.1415f;
	ekf_buffer[5]=euler[2]*180/3.1415f;

// 	printf("\r(ekf)roll:%.3f pitch:%.3f yaw:%.3f   (raw)roll:%.3f pitch:%.3f yaw:%.3f, P:%f,%f,%f,%f\n",ekf_buffer[0],ekf_buffer[1],ekf_buffer[2],ekf_buffer[3],ekf_buffer[4],ekf_buffer[5],
// 		ekf_est.P[6*14], ekf_est.P[6*14], ekf_est.P[6*14], ekf_est.P[6*14]);

// 	printf("%.3f:pos:%f,%f, vel:%f,%f\n", systimer->gettime() / 1000000.0f, ekf_est.ekf_result.Pos_x, ekf_est.ekf_result.Pos_y, ekf_est.ekf_result.Vel_x, ekf_est.ekf_result.Vel_y);

//	float a_body[3] = {accel.array[0], accel.array[1], accel.array[2]};
//	float a_ned[3];
//	ekf_est.tf_ned2body(a_body, a_ned);

// 	extern float a_in_f[3];
// 	float yaw_cf = atan2f(NED2BODY[0][1], NED2BODY[0][0]);
// 	printf("%.3f, %.2f, %.2f, %.2f, %.2f,%.2f, %.2f, %.2f\n", systimer->gettime()/1000000.0f, a_in_f[0], a_in_f[1], -acc_ned[0], -acc_ned[1], ekf_est.ekf_result.Vel_x, ekf_est.ekf_result.Vel_y, accel.array[2]);
// 	float acc_data[6];	// [acc_N_ekf, acc_E_ekf, acc_N_cf, acc_E_cf, V_N_EKF, V_E,EKF];
// 	acc_data[0] = a_in_f[0];
// 	acc_data[1] = a_in_f[1];
// 	acc_data[2] = ekf_est.ekf_result.Vel_x;
// 	acc_data[3] = ekf_est.ekf_result.Vel_y;
// 	acc_data[4] = -acc_ned[0];
// 	acc_data[5] = -acc_ned[1];
// 
// 	log2(acc_data, TAG_ACC_DATA, sizeof(acc_data));

	if (use_EKF > 0.5f)
	{
		euler[0] = radian_add(ekf_est.ekf_result.roll, quadcopter_trim[0]);
		euler[1] = radian_add(ekf_est.ekf_result.pitch, quadcopter_trim[1]);
		euler[2] = radian_add(ekf_est.ekf_result.yaw, quadcopter_trim[2]);
	}
	else
	{
		euler[0] = radian_add(euler[0], quadcopter_trim[0]);
		euler[1] = radian_add(euler[1], quadcopter_trim[1]);
		euler[2] = radian_add(euler[2], quadcopter_trim[2]);
	}
	
	body_rate.array[0] = gyro_reading.array[0] + gyro_bias[0];
	body_rate.array[1] = gyro_reading.array[1] + gyro_bias[1];
	body_rate.array[2] = gyro_reading.array[2] + gyro_bias[2];

	float mag_size = sqrt(mag.array[0]*mag.array[0]+mag.array[1]*mag.array[1]+mag.array[2]*mag.array[2]);
	TRACE("mag_size:%.3f, %.0f, %.0f, %.0f    \n", mag_size, mag.array[0], mag.array[1], mag.array[2]);
	TRACE("euler:%.2f,%.2f,%.2f, systime:%f, bias:%.2f/%.2f/%.2f, pressure=%.2f \n ", euler[0]*PI180, euler[1]*PI180, euler[2]*PI180, systimer->gettime()/1000000.0f, gyro_bias[0]*PI180, gyro_bias[1]*PI180, gyro_bias[2]*PI180, a_raw_pressure);
	TRACE("acc ned:%.2f, %.2f, %.2f\n", acc_ned[0], acc_ned[1], acc_ned[2]);

	TRACE("q cf:%.3f,%.3f,%.3f,%.3f, q ekf: %.3f,%.3f,%.3f,%.3f\n", q0, q1, q2, q3, ekf_est.ekf_result.q0, ekf_est.ekf_result.q1, ekf_est.ekf_result.q2, ekf_est.ekf_result.q3);

	calculate_baro_altitude();

	acc_ned[2] = acc_ned[2];

	alt_estimator.set_land_effect(armed && (!airborne || (!isnan(sonar_distance) && sonar_distance < 0.5f)));
	alt_estimator.set_static_mode(!armed);
	alt_estimator.update(acc_ned[2], a_raw_altitude, interval);
	bool tilt_ok = (euler[0] > -PI/6) && (euler[0] < PI/6) && (euler[1] > -PI/6) && (euler[1] < PI/6);
	alt_estimator2.set_land_effect(armed && (!airborne || (!isnan(sonar_distance) && sonar_distance < 0.5f)));
	alt_estimator2.set_static_mode(!armed);
	alt_estimator2.update(acc_ned[2], a_raw_altitude, tilt_ok ? sonar_distance : NAN, interval);
	estimator.update_accel(-acc_ned[0], -acc_ned[1], systimer->gettime());

	if (new_gps_data)
	{
		// fuse gps data
		if (gps.fix > 2)
		{
			estimator.update_gps(COORDTIMES * gps.latitude, COORDTIMES * gps.longitude, gps.DOP[1]/100.0f, systimer->gettime());

			log_set_time(gps.timestamp);
		}

		if (gps.fix > 2 && gps.DOP[1] < 300)
		{
			float yaw_gps = gps.direction * PI / 180;
			if (yaw_gps > PI)
				yaw_gps -= 2 * PI;

			static float last_gps_data_time = 0;
			static float last_gps_altitude = 0;
			float t = systimer->gettime()/1000000.0f;
			float dt = t - last_gps_data_time;
			last_gps_data_time = t;
			
			float new_ground_speed_east = sin(yaw_gps) * gps.speed;
			float new_ground_speed_north = cos(yaw_gps) * gps.speed;
			
			if (dt > 0 && dt < 1)
			{
				ground_accel_north = (new_ground_speed_north - ground_speed_north) / dt;
				ground_accel_east = (new_ground_speed_east - ground_speed_east) / dt;
				climb_rate_gps = (gps.altitude - last_gps_altitude) / dt;
				gps_attitude_timeout += dt;
			}
			else
			{
				ground_accel_north = 0;
				ground_speed_north = 0;
				ground_accel_east = 0;
				ground_accel_east = 0;
				gps_attitude_timeout = 0;
			}
			
			ground_speed_north = new_ground_speed_north;
			ground_speed_east = new_ground_speed_east;
			last_gps_altitude = gps.altitude;
		}
		else
		{
			gps_attitude_timeout = 0;
		}
	}

	batt.update(voltage, current, interval);

	return 0;
}

int yet_another_pilot::sensor_calibration()
{
	for(int i=0; i<3; i++)
	{
		if (isnan(mag_bias[i]))
			mag_bias[i] = 0;
		if (isnan(mag_scale[i]))
			mag_scale[i] = 1;
	}

	
	// update gyro temperature compensation
	if (!isnan((float)_gyro_bias[0][0]) && !isnan((float)_gyro_bias[1][0]))
	{
		float dt = _gyro_bias[1][0] - _gyro_bias[0][0];
		if (dt > 1.0f)
		{
			for(int i=0; i<3; i++)
			{
				gyro_temp_a.array[i] = _gyro_bias[0][i+1];
				gyro_temp_k.array[i] = (_gyro_bias[1][i+1] - _gyro_bias[0][i+1]) / dt;
			}
			temperature0 = _gyro_bias[0][0];
		}
		else
		{
			// treat as one point
			int group = !isnan(_gyro_bias[0][0]) ? 0 : (!isnan(_gyro_bias[1][0]) ? 1: -1);
			for(int i=0; i<3; i++)
			{
				gyro_temp_a.array[i] = group >= 0 ? _gyro_bias[group][i+1] : 0;
				gyro_temp_k.array[i] = 0;
			}
			temperature0 = group != 0 ? _gyro_bias[group][0] : 0;
		}
	}
	else
	{
		int group = !isnan(_gyro_bias[0][0]) ? 0 : (!isnan(_gyro_bias[1][0]) ? 1: -1);
		for(int i=0; i<3; i++)
		{
			gyro_temp_a.array[i] = group >= 0 ? _gyro_bias[group][i+1] : 0;
			gyro_temp_k.array[i] = 0;
		}
		temperature0 = group != 0 ? _gyro_bias[group][0] : 0;
	}

	// static base value detection
	motion_detector detect_acc;
	motion_detector detect_gyro;
	vector mag_avg = {0};
	//vector accel_avg = {0};
	//vector gyro_avg = {0};
	
	motion_acc.set_threshold(1);
	detect_acc.set_threshold(1);
	detect_gyro.set_threshold(5 * PI / 180);


	int baro_counter = 0;
	ground_pressure = 0;
	int calibrating_count = 900;
	ground_temperature = 0;
	
	int lastt = 0;
	while(detect_acc.get_average(NULL) < calibrating_count && detect_gyro.get_average(NULL) < calibrating_count)
	{
		read_imu_and_filter();
		read_sensors();
		if ((critical_errors & (~int(ignore_error))) & (error_baro | error_gyro | error_magnet | error_accelerometer))
			return -2;
		if (detect_gyro.new_data(gyro_reading))
			return -1;
		if (detect_acc.new_data(accel))
			return -1;
		systimer->delayus(1000);

		printf("\r%d/%d", detect_acc.get_average(NULL), calibrating_count);

		vector_add(&mag_avg, &mag);
		if (new_baro_data)
		{
			baro_counter ++;
			ground_pressure += a_raw_pressure;
			ground_temperature += a_raw_temperature;
		}
		
		if ((systimer->gettime()/1000)%50 > 25)
		{
			SAFE_ON(state_led);
			SAFE_ON(SD_led);
		}
		else
		{
			SAFE_OFF(state_led);
			SAFE_OFF(SD_led);
		}

		if (rgb)
		{
			float color[5][3] = 
			{
				{1,0,0},
				{0,1,0},
				{0,0,1},
				{0.2,0,1},
				{0.2,1,0},
			};

			int i = (systimer->gettime() / 150000) % 5;
			rgb->write(color[i][0], color[i][01], color[i][2]);
		}
		
		STOP_ALL_MOTORS();
	}
	
	vector_divide(&mag_avg, calibrating_count);
	ground_pressure /= baro_counter;
	ground_temperature /= baro_counter;

	LOGE("base value measured\n");
	
	vector accel_avg;
	vector gyro_avg;
	detect_gyro.get_average(&gyro_avg);
	detect_acc.get_average(&accel_avg);



	NonlinearSO3AHRSinit(-accel_avg.V.x, -accel_avg.V.y, -accel_avg.V.z, 
		-mag_avg.V.x, -mag_avg.V.y, mag_avg.V.z, 
		gyro_avg.V.x, gyro_avg.V.y, gyro_avg.V.z);

	ekf_est.init(-1*accel_avg.V.x, -1*accel_avg.V.y, -1*accel_avg.V.z,-1*mag_avg.V.x, -1*mag_avg.V.y, mag_avg.V.z,gyro_avg.V.x, gyro_avg.V.y, gyro_avg.V.z);
	
	return 0;
}

int yet_another_pilot::set_mode(copter_mode newmode)
{
	if (!armed)
		newmode = invalid;
	if (newmode == flight_mode)
		return 0;

	int error = 0;

	switch(newmode)
	{
	case basic:
		error = mode_basic.setup();
		break;
	case althold:
		error = mode_althold.setup();
		break;
	case poshold:
		error = mode_poshold.setup();
		break;
	case optical_flow:
		error = mode_of_loiter.setup();
		break;
	case RTL:
		error = mode_RTL.setup();
		break;
	}
	
	if (error >= 0)
	{
		flight_mode = newmode;
		LOGE("new flight mode: %d\n", flight_mode);
	}
	else
	{
		LOGE("FAILED entering %d mode\n", newmode);
	}

	return error;
}

int yet_another_pilot::arm(bool arm /*= true*/)
{
	if (arm == armed)
		return 0;
	if (arm)
	{
		if (mag_calibration_state)
		{
			LOGE("arm failed: calibrating magnetometer\n");
			return -1;
		}

		if (lowpower > 1)
		{
			LOGE("arm failed: power critical\n");
			return -2;
		}

		if (use_EKF > 0.5f && !ekf_est.ekf_is_ready())
		{
			LOGE("arm failed: EKF not ready\n");
			return -1;
		}

		// emergency switch
		if (rc[4] > -0.8f)
		{
			LOGE("arm failed: emergency switch\n");
			return -1;
		}
	}

	attitude_controll.provide_states(euler, use_EKF > 0.5f ? &ekf_est.ekf_result.q0 : &q0, body_rate.array, motor_saturated ? LIMIT_ALL : LIMIT_NONE, airborne);
	attitude_controll.reset();
	if (use_alt_estimator2 > 0.5f)
	{	
		float alt_state[3] = {alt_estimator2.x[0], alt_estimator2.x[1], alt_estimator2.x[2] + acc_ned[2]};
		alt_controller.provide_states(alt_state, sonar_distance, euler, throttle_real, LIMIT_NONE, airborne);
	}
	else
	{
		float alt_state[3] = {alt_estimator.state[0], alt_estimator.state[1], alt_estimator.state[3] + acc_ned[2]};			
		alt_controller.provide_states(alt_state, sonar_distance, euler, throttle_real, LIMIT_NONE, airborne);
	}
	alt_controller.reset();
	

	takeoff_ground_altitude = alt_estimator.state[0];
	yaw_launch = euler[2];
	collision_detected = 0;
	tilt_us = 0;
	throttle_result = 0;
	airborne = false;
	islanding = false;
	last_position_ready = pos_estimator_ready();
	last_airborne = false;

	// update home
	if (pos_estimator_ready())
	{
		get_pos_velocity_ned(home, NULL);
		set_home(home);
	}
	
	armed = arm;
	execute_mode_switching_from_stick();
	
	LOGE("%s OK\n", arm ? "arm" : "disarm");
	
	return 0;
}

int yet_another_pilot::disarm()
{
	return arm(false);
}

void yet_another_pilot::reset_mag_cal()
{
	LOGE("start magnetometer calibrating");
	mag_calibrator.reset();
	mag_calibration_state = 1;
}

void yet_another_pilot::cancel_mag_cal()
{
	LOGE("cancel magnetometer calibrating\n");
	mag_calibration_state = 0;
}

void yet_another_pilot::reset_accel_cal()
{
	memset(acc_avg_count, 0, sizeof(acc_avg_count));
	acc_cal_requested = true;
	acc_cal_done = false;
}

int yet_another_pilot::finish_accel_cal()
{
	if (!acc_cal_requested || acc_cal_done)
		return 0;

	for(int i=0; i<6; i++)
		if (acc_avg_count[i] < 100)
			return -(i+1);

	// calculate
	gauss_newton_sphere_fitting fitter;
	float data[6*3];
	for(int i=0; i<6; i++)
	{
		data[i*3+0] = acc_calibrator[i].array[0];
		data[i*3+1] = acc_calibrator[i].array[1];
		data[i*3+2] = acc_calibrator[i].array[2];
	}

	fitter.calculate(data, 6);
	fitter.get_result(data);	// bias[0~2], scale[0~2]

	data[3] *= G_in_ms2;
	data[4] *= G_in_ms2;
	data[5] *= G_in_ms2;
	
	// apply and save
	acc_bias[0] = data[0];
	acc_bias[1] = data[1];
	acc_bias[2] = data[2];
	acc_scale[0] = data[3];
	acc_scale[1] = data[4];
	acc_scale[2] = data[5];

	acc_bias[0].save();
	acc_bias[1].save();
	acc_bias[2].save();
	acc_scale[0].save();
	acc_scale[1].save();
	acc_scale[2].save();

	LOGE("acc bias: %.3f, %.3f, %.3f, scale: %.3f, %.3f, %.3f\n", data[0], data[1], data[2], data[3], data[4], data[5]);
	acc_cal_done = true;

	return 0;
}

int yet_another_pilot::execute_mode_switching_from_stick()
{
	copter_mode mode = mode_from_stick();

	if (!airborne && mode != basic)
		mode = althold;

	if (!pos_estimator_ready() && mode == poshold)
		mode = optical_flow;

	return set_mode(mode);
}

int yet_another_pilot::handle_mode_switching()
{
	copter_mode stick_mode = mode_from_stick();

	// did stick mode changed?
	if (last_mode_from_switch != stick_mode)
	{
		new_event(event_mode_switch_changed, 0);

		last_mode_from_switch = stick_mode;

		LOGE("mode switch changed to %d\n", stick_mode);

		execute_mode_switching_from_stick();
	}

	// position ready state changed?
	if (pos_estimator_ready() != last_position_ready)
	{
		new_event(pos_estimator_ready() ? event_pos_ready : event_pos_bad, 0);

		last_position_ready = pos_estimator_ready();

		LOGE("position estimator state changed: %s\n", last_position_ready ? "ready" : "failed");

		execute_mode_switching_from_stick();
	}

	// airborne state changed?
	if (!last_airborne && airborne)
	{
		last_airborne = true;

		LOGE("airborne!\n", last_position_ready ? "ready" : "failed");

		execute_mode_switching_from_stick();
	}

	return 0;
}

copter_mode yet_another_pilot::mode_from_stick()
{
	static copter_mode stick_mode = althold;
	if (rc[5] < -0.6f)
		stick_mode = basic;
	else if (rc[5] > 0.6f)
		stick_mode = poshold;
	else if (rc[5] > -0.5f && rc[5] < 0.5f)
		stick_mode = althold;

	return stick_mode;
}

int yet_another_pilot::new_event(int event, int arg)
{
	int max_count = countof(events);

	if (event_count >= max_count)
		return -1;

	events[event_count] = event;
	events_args[event_count++] = arg;

	return 0;
}

int yet_another_pilot::handle_events()
{
	for(int i=0; i<event_count; i++)
	{
		int event = events[i];
		int arg = events_args[i];

		// TODO;
	}

	event_count = 0;

	return 0;
}


static float takeoff_arming_time = 0;
static bool is_taking_off=false;
int yet_another_pilot::start_taking_off()
{
	if (armed)
		return -1;

	LOGE("\nauto take off \n");
	arm();
	alt_controller.set_altitude_target(alt_controller.get_altitude_state()-2.0f);
	LOGE("\narmed!\n");
	//1s=1000000us		
	takeoff_arming_time=systimer->gettime();
	is_taking_off=true;

	return 0;
}

int yet_another_pilot::check_stick_action()
{
	if (critical_errors & (~int(ignore_error)) )
	{
		disarm();
		return -1;
	}

	// flashlight toggle:
	// flips mode switch twice in 1 seconds
	static float last_ch5 = 0;
	if (fabs(rc[5]-last_ch5) > 0.20f)
	{
		last_ch5 = rc[5];
		static int ch5_flip_count = 0;
		static int64_t last_ch5_flip_time = 0;

		if (systimer->gettime() - last_ch5_flip_time < 1000000)
			ch5_flip_count ++;
		else
			ch5_flip_count = 1;
		last_ch5_flip_time = systimer->gettime();

		if (ch5_flip_count & 2 && flashlight)
		{
			LOGE("toggle flashlight\n");
			if (flashlight)
				flashlight->toggle();
		}

	}

	// emergency switch
	// magnetometer calibration starts if flip emergency switch 10 times, interval systime between each flip should be less than 1 second.
	static float last_ch4 = NAN;
	if (g_pwm_input_update[4] > systimer->gettime() - 500000)
	{		
		if (isnan(last_ch4))
			last_ch4 = rc[4];
		if (fabs(rc[4]-last_ch4) > 0.20f)
		{
			disarm();
			last_ch4 = rc[4];
			LOGE("shutdown!\n");
			
			static int flip_count = 0;
			static int64_t last_flip_time = 0;
			
			if (systimer->gettime() - last_flip_time < 1000000)
				flip_count ++;
			else
				flip_count = 1;
			last_flip_time = systimer->gettime();
			
			if (flip_count == 10)
			{
				if (mag_calibration_state)
					cancel_mag_cal();
				else
					reset_mag_cal();
			}
		}
	}
	else
	{
		last_ch4 = NAN;
	}

	// arm action check: RC first four channel active, throttle minimum, elevator stick down, rudder max or min, aileron max or min, for 0.5second
	static int64_t arm_start_tick = 0;
	static int64_t arm_command_sent = false;
	bool arm_action = rc_fail>=0 && rc[2] < 0.1f  && fabs(rc[0]) > 0.85f
					&& fabs(rc[1]) > 0.85f && fabs(rc[3]) > 0.85f;
	if (!arm_action)
	{
		arm_start_tick = 0;
		arm_command_sent = false;
	}
	else
	{
		if (arm_start_tick > 0 && !arm_command_sent)
		{
			if (systimer->gettime() - arm_start_tick > 500000)
			{
				arm_command_sent = true;
				
				if (armed)
				{
					if (airborne)
					{
						LOGE("airborne, stick disarm denied!\n");
					}
					else
					{
						disarm();
						LOGE("stick disarm!\n");
					}
				}
				else
				{
					arm();
					LOGE("stick arm!\n");
				}
			}
		}
		else
		{
			arm_start_tick = systimer->gettime();
		}
	}

	// takeoff/landing switch
	static float last_ch7 = rc[7];
	if (fabs(rc[7]-last_ch7) > 0.20f)
	{
		last_ch7 = rc[7];
		static int flip_count = 0;
		static int64_t last_flip_time = 0;
		
		if (systimer->gettime() - last_flip_time < 1000000)
			flip_count ++;
		else
			flip_count = 0;
		last_flip_time = systimer->gettime();

		if(airborne && flip_count >=1)
		{
			flip_count=0;
			LOGE("\nauto landing!\n");
			islanding=true;
		}
		else if(!is_taking_off && !armed && flip_count >=1)
		{
			flip_count=0;
			start_taking_off();
		}
	}
	return 0;
}

void yet_another_pilot::handle_takeoff()
{
	if(is_taking_off && systimer->gettime()>=takeoff_arming_time+2000000)
	{
		is_taking_off=false;
		alt_controller.set_altitude_target(alt_controller.get_altitude_state()+1.0f);
		LOGE("\nalread take off\n");
	}
}

int64_t land_detect_us = 0;
int yet_another_pilot::land_detector()
{
	land_possible = throttle_result < 0.2f && fabs(alt_estimator.state[1]) < (quadcopter_max_descend_rate/4.0f);

	if ((rc[2] < 0.1f || (islanding && throttle_result < 0.2f))					// landing and low throttle output, or just throttle stick down
		&& fabs(alt_estimator.state[1]) < (quadcopter_max_descend_rate/4.0f)	// low climb rate : 25% of max descend rate should be reached in such low throttle, or ground was touched
		&& (!alt_controller.used() || (alt_controller.target_climb_rate < 0 && alt_estimator.state[1] > alt_controller.target_climb_rate))	// alt controller not running or can't reach target descending rate
// 		&& fabs(alt_estimator.state[2] + alt_estimator.state[3]) < 0.5f			// low acceleration
	)
	{
		land_detect_us = land_detect_us == 0 ? systimer->gettime() : land_detect_us;

		if (systimer->gettime() - land_detect_us > (airborne ? 1000000 : 15000000))		// 15 seconds for before take off, 1 senconds for landing
		{
			disarm();
			LOGE("landing detected");
		}
	}
	else
	{
		land_detect_us = 0;
	}
	
	return 0;
}

int yet_another_pilot::crash_detector()
{
	// always detect high G force
	vector accel_delta;
	
	accel_delta.array[0] = accel.array[0]/G_in_ms2 - 2 * halfvx;
	accel_delta.array[1] = accel.array[1]/G_in_ms2 - 2 * halfvy;
	accel_delta.array[2] = accel.array[2]/G_in_ms2 - 2 * halfvz;

	float gforce = vector_length(&accel_delta);
	
	static float gforce_sum = 0;
	static float gforce_sum2 = 0;
	static int gforce_count = 0;
	float gforce_avg = 1.0f;
	float gforce_variance = 1.0f;
	if (airborne)
	{
		gforce_count ++;
		gforce_sum += gforce;
		gforce_sum2 += gforce * gforce;
	}
	if (gforce_count > 0)
	{
		gforce_avg =  gforce_sum / gforce_count;
		gforce_variance = gforce_sum2 / gforce_count - gforce_avg * gforce_avg;
	}
	
	if (gforce > 3.75f)
	{
		LOGE("high G force (%.2f) detected\n", gforce);
		collision_detected = systimer->gettime();
	}

	// forced shutdown if >7g external force
	if (gforce > 5.5f)
	{
		LOGE("very high G force (%.2f) detected (%.0f,%.0f,%.0f)\n", gforce, accel.array[0], accel.array[1], accel.array[2]);
		disarm();
	}

	int prot = (float)::crash_protect;

	bool landing_requested = rc[2] < 0.1f || (islanding && throttle_result < 0.2f);
		
	// tilt detection
	float cos0 = cos(euler[0]);
	float cos1 = cos(euler[1]);
	float tilt = sqrt(cos0*cos0 + cos1*cos1);
	if (tilt < 0.33f || cos0 < 0 || cos1 < 0)		// around 70 degree
		tilt_us = tilt_us > 0 ? tilt_us : systimer->gettime();
	else
		tilt_us = 0;

	if ((collision_detected > 0 && systimer->gettime() - collision_detected < 10000) && (landing_requested)) 
	{
		LOGE("landing impact detected(%s)\n", (collision_detected > 0 && systimer->gettime() - collision_detected < 5000000) ? "collision" : "tilt");

		disarm();
	}

	if (tilt_us> 0 && systimer->gettime()-tilt_us > 2000000)	// flip over more than 2 second
	{
		LOGE("flip over for more than 2 senconds");

		disarm();
	}

	return 0;
}

float yet_another_pilot::ppm2rc(float ppm, float min_rc, float center_rc, float max_rc, bool revert)
{
	float v = (ppm-center_rc) / (ppm>center_rc ? (max_rc-center_rc) : (center_rc-min_rc));

	v = limit(v, -1, +1);

	if (revert)
		v = -v;

	return v;
}

int yet_another_pilot::handle_cli(IUART *uart)
{
	if (!uart)
		return -1;
	
	char line[1024];
	char out[1024];
	int byte_count = uart->readline(line, sizeof(line));
	if (byte_count <= 0)
		return 0;
	
	line[byte_count-1] = 0;
	
	int out_count = parse_command_line(line, out);
	if (out_count>0)
	{
		out[out_count] = 0;
		TRACE("%s,%s\n", line, out);
		uart->write(out, out_count);
	}

	return 0;
}

uint32_t __attribute__((weak)) get_chip_id_crc32()
{
	const void *stm32_id_address = (const void*)0x1FFFF7E8;
	char data[12];
	memcpy(data, stm32_id_address, 12);

	return crc32(0, data, 12);
}

int yet_another_pilot::handle_wifi_controll(IUART *uart)
{
	if (!uart)
		return -1;
	
	char line[1024];
	char out[1024];
	int byte_count = uart->readline(line, sizeof(line));
	if (byte_count <= 0)
		return 0;
	
	line[byte_count] = 0;
	int len = strlen(line);
	const char * keyword = ",stick\n";
	const char * keyword2 = "stick,";
	const char * keyword3 = "flashlight";
	
	if (strstr(line, keyword2) == line)
	{
		if (sscanf(line+strlen(keyword2), "%f,%f,%f,%f", &rc_mobile[0], &rc_mobile[1], &rc_mobile[2], &rc_mobile[3] ) == 4)
		{
			mobile_last_update = systimer->gettime();
			TRACE("stick:%f,%f,%f,%f\n", rc_mobile[0], rc_mobile[1], rc_mobile[2], rc_mobile[3]);
			
		}
	}
	else if (strcmp(line, "@\n") == 0 || strcmp(line, "@\r\n") == 0)
	{
		rc_mobile[0] = 0;
		rc_mobile[1] = 0;
		rc_mobile[2] = 0.5;
		rc_mobile[3] = 0;

		mobile_last_update = systimer->gettime();
	}

	else if (strstr(line, keyword) == (line+len-strlen(keyword)))
	{
		if (sscanf(line, "%f,%f,%f,%f", &rc_mobile[0], &rc_mobile[1], &rc_mobile[2], &rc_mobile[3] ) == 4)
		{
			mobile_last_update = systimer->gettime();
			TRACE("stick:%f,%f,%f,%f\n", rc_mobile[0], rc_mobile[1], rc_mobile[2], rc_mobile[3]);
			
			rc_mobile[0] *= 1.33f;
			rc_mobile[1] *= 1.33f;
			rc_mobile[2] = (rc_mobile[2] - 0.5f) * 1.35f + 0.5f;
			rc_mobile[3] *= 1.33f;
			
			TRACE("rc_mobile: %f, %f, %f, %f\n", rc_mobile[0], rc_mobile[1], rc_mobile[2], rc_mobile[3]);
		}
	}

	else if (strstr(line, "arm") == line)
	{
		LOGE("mobile arm\n");
		arm();
		alt_controller.set_altitude_target(alt_controller.get_altitude_state()-2.0f);

		const char *armed = "arm,ok\n";
		uart->write(armed, strlen(armed));
	}

	else if (strstr(line, "disarm") == line)
	{
		LOGE("mobile disarm\n");

		disarm();
		const char *disarmed = "disarm,ok\n";
		uart->write(disarmed, strlen(disarmed));
	}

	else if (strstr(line, "hello") == line)
	{
		char out[200];
		sprintf(out, "hello,yap(%s)(bsp %s)\n", version_name, bsp_name);
		uart->write(out, strlen(out));
	}

	else if (strstr(line, "chip_id") == line)
	{
		char out[200];
		sprintf(out, "chip_id,%08X\n", get_chip_id_crc32());
		uart->write(out, strlen(out));
	}

	else if (strstr(line, "takeoff") == line)
	{
		const char *tak = "takeoff,ok\n";
		uart->write(tak, strlen(tak));
		start_taking_off();
	}

	else if (strstr(line, "land") == line)
	{
		LOGE("mobile land\n");
		const char *land = "land,ok\n";
		uart->write(land, strlen(land));

		islanding = true;
	}
	
	else if (strstr(line, "RTL_get") == line)
	{
		char tmp[50];
		sprintf(tmp, "RTL_get,%d\n", flight_mode == RTL ? 1 : 0);
		uart->write(tmp, strlen(tmp));
	}

	else if (strstr(line, "RTL") == line)
	{
		LOGE("mobile RTL\n");
		const char *rtl = "RTL,ok\n";
		uart->write(rtl, strlen(rtl));
		set_mode(RTL);
	}

	else if (strstr(line, "STOPRTL") == line)
	{
		LOGE("mobile stop RTL\n");
		const char *rtl = "STOPRTL,ok\n";
		uart->write(rtl, strlen(rtl));
		execute_mode_switching_from_stick();
	}

	else if (strstr(line, "state") == line)
	{
		float pos[2] = {0};
		float velocity[2] = {0};

		bool pos_ready = false;
		if (home_set)
		{
			if (pos_ready = get_pos_velocity_ned(pos, velocity) == 0)
			{
				pos[0] -= home[0];
				pos[1] -= home[1];
			}
		}
		float distance = sqrt(pos[0] * pos[0] + pos[1] * pos[1]);
		float v = sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		float battery = (batt.get_internal_voltage() - 10.7f)/(12.4f-10.7f);
		battery = limit(battery, 0, 1);

		sprintf(out, 
			"state,%f,%f,%s,%.1f,%.1f,"		// gps latitude, longitude, gps ready, altitude, vertical velocity,
			"%.1f,%.2f,"			// horizontal distance, horizontal velocity, 
			"%.1f,%.1f,%.1f,"	// attitude: roll, pitch, yaw
			"%s,%s,"			// airborne, sonar actived
			"%.2f,%.1f,",			// battery, 
			gps.latitude, gps.longitude, pos_ready ? "1" : "0", alt_estimator.state[0]-takeoff_ground_altitude, alt_estimator.state[1],
			distance, v,
			euler[0] * 180 / PI, euler[1] * 180 / PI, euler[2] * 180 / PI,
			airborne ? "1" : "0", alt_controller.sonar_actived() ? "1" : "0",
			battery, batt.get_internal_voltage()
			);

		uart->write(out, strlen(out));
		sprintf(out, 
			"%d,%d\n",			// RLT, flashlight states
			flight_mode == RTL ? 1 : 0, flashlight ? (flashlight->get() ? 1 : 0) : 0
			);
		uart->write(out, strlen(out));
			
	}

	else if (strstr(line, "param") == line)
	{
		char out[200];
		float limV = *find_param("limV");
		float limH = *find_param("limH");
		float maxH = *find_param("maxH");
		float maxD = *find_param("maxD");
		float maxC = *find_param("maxC");
		float raty = *find_param("raty");

		sprintf(out, "param,%.2f,%.2f,%.2f,%.2f,%.2f,%f\n",
			limV, limH, maxH, maxD, maxC, raty);

		uart->write(out, strlen(out));
	}
	
	else if (strstr(line, "mag_cal_state") == line)
	{
		mag_calibration_stage stage = mag_calibrator.get_stage();
		int stage_res = mag_calibration_state ? (stage+1) : 0;
		if (stage_res == 6)
			stage_res = 0;
		sprintf(out, "mag_cal_state,%d,%d\n", stage_res, last_mag_calibration_result);
		uart->write(out, strlen(out));
	}

	else if (strstr(line, "mag_cal") == line)
	{
		reset_mag_cal();
		uart->write("mag_cal,ok\n", 11);
	}	
	else if (strstr(line, "cancel_mag_cal") == line)
	{
		cancel_mag_cal();
		uart->write("cancel_mag_cal,ok\n", 11);
	}	
	
	else if (strstr(line, "acc_cal_state\n") == line)
	{
		if (!acc_cal_requested)
			uart->write("-1\n", 3);
		else
		{
			int acc_state = 0;
			for(int i=0; i<6; i++)
			{
				if (yap.acc_avg_count[i] > 400)
					acc_state |= (1<<i);
			}

			sprintf(out, "%d\n", acc_state);
			uart->write(out, strlen(out));
		}
	}
	
	else if (strstr(line, "acc_cal\n") == line)
	{
		reset_accel_cal();
		uart->write("acc_cal,ok\n", 3);
	}


	else if (strstr(line, "flashlight_get") == line)
	{
		char tmp[50];
		if (!flashlight)
			strcpy(tmp, "flashlight_get,0\n");
		else
			sprintf(tmp, "flashlight_get,%d\n", flashlight->get() ? 1 : 0);
		uart->write(tmp, strlen(tmp));
	}

	else if (strstr(line, "flashlight") == line)
	{
		const char * comma = strstr(line, ",");
		if (!comma)
			SAFE_OFF(flashlight);
		
		if (comma && atoi(comma+1))
		{
			SAFE_ON(flashlight);
		}
		else
		{
			SAFE_OFF(flashlight);
		}
		
		uart->write("flashlight\n", 11);
	}
	else if (strstr(line, "set,") == line)
	{
		const char * comma = strstr(line+4, ",");
		if (!comma)
		{
			uart->write("set,,fail\n", 10);
			return -1;
		}

		char para_name[5] = {0};
		strncpy(para_name, line+4, 4);

		float *pv = param::find_param(para_name);

		if (pv)
		{
			float v = atof(comma+1);
			if (strstr(comma+1, "NAN"))
				v = NAN;
			*pv = v;
			param(para_name, 0).save();
			uart->write("set,", 4);
			uart->write(para_name, strlen(para_name));
			uart->write(",ok\n", 4);
			return 3;
		}
		else
		{
			uart->write("set,,not found\n", 16);
			return -1;
		}
	}

	else if (strstr(line, "get,") == line)
	{
		char para_name[5] = {0};
		strncpy(para_name, line+4, 4);

		float *pv = param::find_param(para_name);
		if (pv)
		{
			char out[50];
			if (isnan(*pv))
				strcpy(out, "NAN\n");
			else
				sprintf(out, "%f\n", *pv);

			uart->write("get,", 4);
			uart->write(para_name, strlen(para_name));
			uart->write(",,", 1);
			uart->write(out, strlen(out));
		}
		else
		{
			uart->write("get,", 4);
			uart->write(para_name, strlen(para_name));
			uart->write(",fail\n", 6);
			return -1;
		}

	}
	
	else
	{
		// invalid packet
		LOGE("invalid packet %s", line);
	}

	return 0;
}

int yet_another_pilot::read_rc()
{
	rcin->get_channel_data(g_pwm_input, 0, 8);
	rcin->get_channel_update_time(g_pwm_input_update, 0, 8);
	TRACE("\rRC");
	int rc2_update_time = systimer->gettime() - g_pwm_input_update[2];
	for(int i=0; i<8; i++)
	{
		rc[i] = ppm2rc(g_pwm_input[i], rc_setting[i][0], rc_setting[i][1], rc_setting[i][2], rc_setting[i][3] > 0);
		TRACE("%.2f,", rc[i]);
	}
	
	rc[2] = (rc[2]+1)/2;
	
	// check rf fail
	// rc no signel for 0.5 seconds, or -10% or less throttle
	bool rf_fail = rcin->state() == RCIN_Fail;
	for(int i=0; i<4; i++)
		if (g_pwm_input_update[i] < systimer->gettime() - 500000)
			rf_fail = true;
	
	if (g_pwm_input[2] < (rc_setting[2][0] - (rc_setting[2][2] - rc_setting[2][0])/10))
		rf_fail = true;
	
	// controll source selecting
	if (rf_fail || forced_mobile_controll > 0.5f)
	{
		if (systimer->gettime() - mobile_last_update < 1000000)
		{
			for(int i=0; i<4; i++)
			{
				rc[i] = rc_mobile[i];
				TRACE("%.2f,", rc_mobile[i]);
			}
			rc_fail = forced_mobile_controll > 0.5f ? 2 : 1;

			rc[4] = -1;	// "emergency switch"
			rc[5] = 1;	// use "poshold" mode
		}
		else
		{
			rc[0] = 0;
			rc[1] = 0;
			rc[2] = 0.5;
			rc[3] = 0;
			rc[5] = 1;	// use "poshold" mode

			rc_fail = -1;

		}
	}
	else
	{
		rc_fail = 0;
	}

	// total RC failure handling
	if (rc_fail < 0)
	{
		float new_rc_fail_tick = rc_fail_tick + interval;
		if (new_rc_fail_tick > 3.0f && rc_fail_tick <= 3.0f)
		{
			LOGE("rc fail, RTL\n");
			if (set_mode(RTL) < 0)
			{
				LOGE("RTL failed, landing\n");
				islanding = true;
			}
		}
		rc_fail_tick = new_rc_fail_tick;
	}
	else
	{
		rc_fail_tick = 0;
	}

	// send RC fail event
	static int last_rc_fail = 0;
	if (rc_fail != last_rc_fail)
	{
		switch(rc_fail)
		{
		case 0:
			LOGE("RC restored");
			break;
		case -1:
			LOGE("RC failed");
			break;
		case 1:
		case 2:
			LOGE("RC from mobile");
			break;
		}
		last_rc_fail = rc_fail;
	}
	
	return 0;
}

int yet_another_pilot::lowpower_handling()
{
	if (interval > 0.1f || isnan(voltage) || isnan(current))
		return -1;
	
	static float lowpower1 = 0;
	static float lowpower2 = 0;

#if 0
	float ref_voltage = voltage;
	float low_voltage1 = 10.8f;
	float low_voltage2 = 10.2f;
#else
	float ref_voltage = batt.get_internal_voltage();
	float delta = fmax(0, alt_estimator.state[0] - takeoff_ground_altitude) * 0.002f;
	float low_voltage1 = 11.0f + delta;
	float low_voltage2 = 10.8f + delta;
#endif

	if (ref_voltage > 6 && ref_voltage < low_voltage2)
	{
		lowpower2 += interval;
	}
	
	else if (ref_voltage > 6 && ref_voltage<low_voltage1)
	{
		lowpower1 += interval;
		lowpower2 = 0;
	}

	else
	{
		lowpower1 = 0;
		lowpower2 = 0;
	}

	if (lowpower1 > 3)
		lowpower = max(lowpower, 1);

	if (lowpower2 > 3)
		lowpower = max(lowpower, 2);

	if (lowpower >= 2)
		islanding = true;
	
	return 0;
}

int yet_another_pilot::stupid_joystick()
{
	IUART * telemetry = manager.getUART("power");
	if (telemetry)
	{
		static int64_t last_send = 0;
		if (systimer->gettime() - last_send > 100000)
		{
			last_send = systimer->gettime();
			uint8_t d[3] = {0x1, lowpower};
			d[2] = d[0] ^ d[1];
			telemetry->write(d, 3);
		}
		
		uint8_t rx[3];
		int count = 0;
		static int64_t last_packet = 0;
		while(telemetry->available() >= 3)
		{
			last_packet = systimer->gettime();
			telemetry->read(rx, 1);
			if (rx[0] != 1)
				continue;
			telemetry->read(rx+1, 2);
			count = 3;
		}
		
		// clear all buffer if no packet arrive for 0.5 second
		if (systimer->gettime() - last_packet > 500000)
		{
			char tmp[100];
			telemetry->read(tmp, 100);
		}
		
		
		if (rx[0] == 1 && rx[2] == (rx[0]^rx[1]))
		{
			if (rx[1])
			{
				LOGE("flashlight on by telemtry");
				SAFE_ON(flashlight);
			}
			else
			{
				LOGE("flashlight off by telemtry");
				SAFE_OFF(flashlight);
			}
		}
		else if (count == 3)
		{
			LOGE("stupid telmetry rx: %d,%d,%d", rx[0], rx[1], rx[2]);
		}
	}
	
	return 0;
}

int yet_another_pilot::light_words()
{
	// critical errors
	int a = ~int(ignore_error);
	if (critical_errors & (~int(ignore_error)))
	{
		static int last_critical_errors = 0;
		if (last_critical_errors != critical_errors)
		{
			last_critical_errors = critical_errors;
			LOGE("critical_errors : %x (", critical_errors);
			for(int i=0; (1<<i)<error_MAX; i++)
			{
				if ((1<<i) & critical_errors)
					LOGE("%s %s", i==0?"":" | ", critical_error_desc[i]);
			}
			LOGE(" )\n");
		}

		int error_code_count = 0;
		for(int i=0; (1<<i)<error_MAX; i++)
			error_code_count++;

		static int64_t t0 = 0;
		if (t0 == 0)
			t0 = systimer->gettime();
		int64_t t = (systimer->gettime()-t0) / 1000;
		int light = (t / 500) % (error_code_count+3);
		if (rgb)
		{
			if (t % 500 < 250 || light >=  error_code_count)
			{
				rgb->write(0,0,0);
			}
			else
			{
				if ((1<<light) & critical_errors)
				{
					rgb->write(1,0,0);
				}
				else
				{
					rgb->write(0,1,0);
				}
			}
		}
	}

	else if (lowpower)			// low voltage warning
	{
		// fast red flash (10hz)
		int dt = lowpower == 2 ? 50000 : 500000;
		int64_t systime = systimer->gettime();
		if (rgb && mag_calibration_state == 0)
		if (systime % (dt*2) < dt)
			rgb->write(1,0,0);
		else
			rgb->write(0,0,0);
	}
	else if (flight_mode == RTL)
	{
		// purple light for RTL
		if (rgb && mag_calibration_state == 0)
			rgb->write(1,0,1);
	}
	else		// normal flashlight, double flash if SDCARD running, single flash if SDCARD failed
	{
		float color[3];

		if (rc_fail < 0)
		{
			// blue flash for RC failure.
			color[0] = 0;
			color[1] = 0;
			color[2] = 1;
		}

		else if (pos_estimator_ready() && mode_from_stick() == poshold)
		{
			// green flash for poshold mode and pos estimator ready.
			color[0] = 0;
			color[1] = 1;
			color[2] = 0;
		}
		else
		{
			// yellow flash for other state
			color[0] = 1;
			color[1] = 1;
			color[2] = 0;
		}

		int64_t systime = systimer->gettime();
		int time_mod_1500 = (systime%1500000)/1000;
		if (time_mod_1500 < 20 || (time_mod_1500 > 200 && time_mod_1500 < 220 && log_ready()))
		{
			if (rgb && mag_calibration_state == 0)
				rgb->write(color[0], color[1], color[2]);
		}
		else
		{
			if(rgb && mag_calibration_state == 0)
				rgb->write(0,0,0);
		}
	}
	
	return 0;
}

void yet_another_pilot::mag_calibrating_worker()
{
	mag_calibrator.do_calibration();
	mag_calibration_result result;
	last_mag_calibration_result = mag_calibrator.get_result(&result);
	LOGE("result:%d, bias:%f,%f,%f, scale:%f,%f,%f\n, residual:%f/%f", last_mag_calibration_result, result.bias[0], result.bias[1], result.bias[2], result.scale[0], result.scale[1], result.scale[2], result.residual_average, result.residual_max);
	
	// do checks and flash RGB LED if error occured
	if (last_mag_calibration_result == 0)
	{
		LOGE("mag calibration success\n");
		
		for(int i=0; i<3; i++)
		{
			mag_bias[i] = result.bias[i];
			mag_scale[i] = result.scale[i];
			mag_bias[i].save();
			mag_scale[i].save();
		}		
		
		// flash RGB LED to indicate a successs
		for(int i=0; i<10; i++)
		{
			if (rgb)
			{
				systimer->delayms(300);				
				rgb->write(0,1,0);
				systimer->delayms(300);
				rgb->write(0,0,0);
			}
		}

		// TODO: update ahrs state
	}
	else
	{
		// flash RGB LED to indicate a failure
		LOGE("mag calibration failed\n");
		for(int i=0; i<10; i++)
		{
			if (rgb)
			{
				systimer->delayms(300);				
				rgb->write(1,0,0);
				systimer->delayms(300);
				rgb->write(0,0,0);
			}
		}
	}

	// log
	mag_calibration_data d =
	{
		{result.bias[0] * 10, result.bias[1] * 10, result.bias[2] * 10},
		{result.scale[0] * 1000, result.scale[1] * 1000, result.scale[2] * 1000},
		result.residual_average * 1000,
		result.residual_max * 1000,
		result.residual_min * 1000,
		last_mag_calibration_result,
		result.num_points_collected,
	};

	// assume blocking async worker won't do any harm
	int64_t timestamp = systimer->gettime();
	while (log_ready() && log(&d, TAG_MAG_CALIBRATION_DATA, timestamp) != 0)
		LOGE("writing log failed");// retry

	// ends
	mag_calibration_state = 0;
}

void yet_another_pilot::main_loop(void)
{
	// calculate systime interval
	static int64_t tic = 0;
	int64_t round_start_tick = systimer->gettime();
	interval = (round_start_tick-last_tick)/1000000.0f;
	last_tick = round_start_tick;
	
	// rc inputs
	read_rc();

	SAFE_OFF(state_led);
	
	// performance counter
	cycle_counter++;
	if (systimer->gettime() - tic > 1000000)
	{
		tic = systimer->gettime();
		LOGE("speed: %d(%d), systime:%.2f\r\n", cycle_counter, round_running_time, systimer->gettime()/1000000.0f);
		loop_hz = cycle_counter;
		cycle_counter = 0;
	}
	
	// RC modes and RC fail detection, or alternative RC source
	handle_mode_switching();
	check_stick_action();
	handle_takeoff();
	handle_wifi_controll(manager.getUART("Wifi"));
	handle_events();
	stupid_joystick();

	// read sensors
	int64_t read_sensor_cost = systimer->gettime();
	//read_sensors();
	read_sensors();
	read_sensor_cost = systimer->gettime() - read_sensor_cost;
	
	// provide mag calibration with data
	if (mag_calibration_state == 1)
	{
		// update RGB LED for user interaction
		if (rgb)
		{
			if (mag_calibrator.get_stage() == stage_horizontal)
				rgb->write(1,0,0);
			else if (mag_calibrator.get_stage() == stage_vertical_pitch)
				rgb->write(0,1,0);
			else if (mag_calibrator.get_stage() == stage_vertical_roll)
				rgb->write(0,0,1);
			else
				rgb->write(0,0,0);
		}

		
		// data
		mag_calibrator.provide_data(mag_uncalibrated.array, euler, body_rate.array, interval);
	
		// make a async call if data ready and change mag_calibration_state to calibrating
		if (mag_calibrator.get_stage() == stage_ready_to_calibrate)
		{
			mag_calibration_state = 2;
			manager.get_asyncworker()->add_work(mag_calibrating_worker_entry, (int)this);
		}
	}

	lowpower_handling();

	// light words
	light_words();

	// all state estimating, AHRS, position, altitude, etc
	calculate_state();

	run_controllers();
	output();
	save_logs();
 	handle_cli(vcp);

	if (armed)
	{
		land_detector();
 		crash_detector();
	}
	else
		land_detect_us = 0;

	SAFE_ON(state_led);

	if (log_ready())
	{		
		// flash SD LED at 10hz
		int t = int(systimer->gettime() % 100000);
		if (t < 50000)
		{
			SAFE_ON(SD_led);
		}
		else
		{
			SAFE_OFF(SD_led);
		}
	}

	// unlock imu data
	imu_data_lock = false;

	// update home once position ready
	if (!home_set && pos_estimator_ready())
	{
		get_pos_velocity_ned(home, NULL);
		set_home(home);
	}

	
	round_running_time = systimer->gettime() - round_start_tick;
	TRACE("\r%d/%d", int(read_sensor_cost), round_running_time);
}


void yet_another_pilot::sdcard_logging_loop(void)
{
	static int64_t tick = systimer->gettime();
	int64_t t = systimer->gettime();
	int dt = t-tick;
	if (dt > 15000)
		TRACE("long log interval:%d\n", dt);


	int64_t starttick = systimer->gettime();
	int res = log_flush();
	starttick = systimer->gettime() - starttick;

	if (starttick > 10000)
		TRACE("long log systime:%d\n", int(starttick));

	if (res == 0)
		tick = t;
}
	
int yet_another_pilot::setup(void)
{	
	range_finder = (IRangeFinder *)manager.get_device("sonar");
	state_led = manager.getLED("state");
	SD_led = manager.getLED("SD");
	flashlight = manager.getLED("flashlight");
	rcin = manager.get_RCIN();
	rcout = manager.get_RCOUT();
	rgb = manager.getRGBLED("rgb");
	vcp = manager.getUART("VCP");

	for(int i=0; i<16; i++)
		g_ppm_output[i] = THROTTLE_MAX;
	output_rc();
	
	int64_t t = systimer->gettime();
	while(systimer->gettime() < t + 20000)
		output_rc();
	STOP_ALL_MOTORS();
	
	estimator.set_gps_latency(0);
	attitude_controll.set_quaternion_mode(use_EKF > 0.5f);
	SAFE_ON(flashlight);


	int res;
	do
	{
		res = sensor_calibration();
		if (res == -2)
			break;
	}while (res < 0);

	SAFE_OFF(flashlight);
	read_rc();
		
	// get two timers, one for main loop and one for SDCARD logging loop
	manager.getTimer("mainloop")->set_period(cycle_time);
	manager.getTimer("mainloop")->set_callback(main_loop_entry);
	manager.getTimer("log")->set_period(10000);
	manager.getTimer("log")->set_callback(sdcard_logging_loop_entry);
	manager.getTimer("imu")->set_period(1000);
	manager.getTimer("imu")->set_callback(imu_reading_entry);
	
	return 0;
}

int main()
{
	bsp_init_all();
	
	log_init();
	LOGE("yap(%s)(bsp %s)\n", version_name, bsp_name);
	int i = 0;
	while(param::enum_params(i))
	{
		char name[5] = {0};
		strncpy(name, param::enum_params(i), 4);
		char tmp[200];
		sprintf(tmp, "%s=%f\n", name, (float)param(name, 0));
		log2(tmp, TAG_TEXT_LOG, strlen(tmp));
		log_flush();
		i++;
	}
	while(0)
	{
		float t = systimer->gettime()/5000000.0f * 2 * PI;
		float t2 = systimer->gettime()/15000000.0f * 2 * PI;
		t2 = sin(t2)/2+0.6f;
		float r = t2*(sin(t)/2+0.5f);
		float g = t2*(sin(t+PI*2/3)/2+0.5f);
		float b = t2*(sin(t+PI*4/3)/2+0.5f);
		
		manager.getRGBLED("rgb")->write(r,g,b);
	}
	
	yap.setup();
	while(1)
	{
	}
}

__attribute__((section("ccm"))) yet_another_pilot yap;
