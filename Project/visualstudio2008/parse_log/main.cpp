#include <stdio.h>
#include <conio.h>
#include <stdint.h>
#include <protocol/RFData.h>
#include <Algorithm/ekf_estimator.h>
#include <Algorithm/ahrs.h>
#include <HAL/Interface/IGPS.h>
#include <algorithm/pos_estimator2.h>
#include <algorithm/ekf_ahrs.h>
#include <algorithm/EKFINS.h>
#include <Windows.h>

#define PI 3.1415926

int compare_sat(const void *v1, const void*v2)
{
	SAT *p1 = (SAT*) v1;
	SAT *p2 = (SAT*) v2;

	return p1->cno < p2->cno ? 1 : -1;
}


int main(int argc, char* argv[])
{
	double baro = 1013.26;
	double alt = (1-pow(baro/1013.25, 0.190284)) * 145366.45 / 0.3048;
	double scaling = (double)1013.26 / 1013.25;
	double temp = ((float)40) + 273.15f;
	double a_raw_altitude = 153.8462f * temp * (1.0f - exp(0.190259f * log(scaling)));

	if (argc < 2)		
	{
		printf("usage: parse_log.exe 0001.dat\npress any key to exit...");
		getch();
		return -2;
	}

	char out_name[300];
	sprintf(out_name, "%s.log", argv[1]);

	FILE *in = fopen(argv[1], "rb");
	FILE *out = fopen(out_name, "wb");
	FILE *excel = fopen("Z:\\flow.csv", "wb");

	if (!in)
	{
		printf("failed opening %s\n", argv[1]);
		return -1;
	}

	if (!out)
	{
		printf("failed opening %s\n", out_name);
		return -1;
	}

	int64_t time = 0;
	ppm_data ppm = {0};
	gps_data gps;
	devices::gps_data gps_extra = {0};
	imu_data imu = {0};
	sensor_data sensor = {0};
	quadcopter_data quad = {0};
	quadcopter_data2 quad2 = {0};
	quadcopter_data3 quad3 = {0};
	quadcopter_data4 quad4 = {0};
	quadcopter_data5 quad5 = {0};
	bool sensor_valid = false;
	bool imu_valid = false;
	bool gps_valid = false;
	bool home_set = false;
	double home_lat = 0;
	double home_lon = 0;
	double lat_meter = 0;
	double lon_meter = 0;
	double speed_north = 0;
	double speed_east = 0;
	float on_pos2[12] = {0};
	px4flow_frame frame = {0};
	rc_mobile_data mobile = {0};

	int ssss = sizeof(frame);
	ekf_estimator ekf_est;
	pos_estimator2 pos2;
	ekf_ahrs ahrs;
	EKFINS ins;
	SAT sats[40];
	SAT_header sat={0};
	float avg_cno = 0;
	float top6_cno = 0;
	posc_ext_data posc = {0};

	while (fread(&time, 1, 8, in) == 8)
	{
		uint8_t tag = time >> 56;
		uint16_t tag_ex;
		time = time & ~((uint64_t)0xff << 56);
		char data[65536];
		int size = 24;

		if (tag == TAG_EXTENDED_DATA)		// extended variable length data packets
		{
			if (fread(&tag_ex, 1, 2, in) != 2)
				break;
			if (fread(&size, 1, 2, in) != 2)
				break;
			if (fread(data, 1, size, in) != size)
				break;
		}
		else
		{
			size = 24;
			if (fread(data, 1, 24, in) != 24)
				break;
		}

		// handle packet data here..
		if (tag == TAG_PPM_DATA)
		{
			memcpy(&ppm, data, 24);
		}
		if (tag == TAG_MOBILE_DATA)
		{
			memcpy(&mobile, data, sizeof(mobile));
		}

		if (tag_ex == TAG_EXTRA_GPS_DATA)
		{
			memcpy(&gps_extra, data, size);
		}
		if (tag == TAG_PX4FLOW_DATA)
		{
			memcpy(&frame, data, sizeof(frame));
		}
		if (tag == TAG_QUADCOPTER_DATA2)
		{
			memcpy(&quad2, data, size);
		}
		if (tag == TAG_QUADCOPTER_DATA3)
		{
			memcpy(&quad3, data, size);
		}
		if (tag == TAG_QUADCOPTER_DATA4)
		{
			memcpy(&quad4, data, sizeof(quad4));
		}
		if (tag == TAG_QUADCOPTER_DATA5)
		{
			memcpy(&quad5, data, size);
		}
		if (tag == TAG_QUADCOPTER_DATA)
		{
			memcpy(&quad, data, sizeof(quad));
		}
		if (tag_ex == TAG_POS_ESTIMATOR2 && size == 48)
		{
			memcpy(on_pos2, data, size);
		}
		if (tag_ex == TAG_POSC_DATA)
		{
			memcpy(&posc, data, size);
		}
		else if (tag_ex == TAG_UBX_SAT_DATA)
		{
			memcpy(&sat, data, size);
			memcpy(sats, data+8, size-8);
			qsort(sats, sat.num_sat_visible, sizeof(SAT), compare_sat);

			avg_cno = 0;
			int used_count = 0;
			for(int i=0; i<sat.num_sat_visible; i++)
			{
				if (sats[i].flags & 0x8)
				{
					avg_cno += sats[i].cno;
					used_count++;
				}
			}

			avg_cno /= used_count;

			if (sat.num_sat_visible >=6)
				top6_cno = (sats[0].cno + sats[1].cno + sats[2].cno + sats[3].cno + sats[4].cno + sats[5].cno)/6.0;
			else
				top6_cno = 0;
		}
		else if (tag == TAG_SENSOR_DATA)
		{
			sensor_valid = true;
			memcpy(&sensor, data, sizeof(sensor));
			int throttle = (ppm.out[0] + ppm.out[1] + ppm.out[2] + ppm.out[3])/4;
			static int power_id = 0;
			if (power_id++ % 10 == 0)
				fprintf(out, "CURR, %d, %d, %f, %f, %f, %d, %d,\r\n", int(time/1000), throttle, sensor.voltage/1000.0f, sensor.current/1000.0f, imu.temperature/1.0f, 0, 0);
		}
		else if (tag == TAG_IMU_DATA)
		{
			memcpy(&imu, data, sizeof(imu));

			static int64_t last_time = 0;
			float dt = (time - last_time)/1000000.0f;
			last_time = time;

			float acc[3] = {-imu.accel[0]/100.0f, -imu.accel[1]/100.0f, -imu.accel[2]/100.0f};
			float mag[3] = {-imu.mag[0]/10.0f, -imu.mag[1]/10.0f, imu.mag[2]/10.0f};
			float gyro[3] = {sensor.gyro[0]*PI/18000, sensor.gyro[1]*PI/18000, sensor.gyro[2]*PI/18000};

			float q0 = quad5.q[0];
			float q1 = quad5.q[1];
			float q2 = quad5.q[2];
			float q3 = quad5.q[3];

			float pitch_cf = -asinf(2.f * (q1*q3 - q0*q2));
			float roll_cf = atan2f(2.f * (q2*q3 + q0*q1), q0*q0 - q1*q1 - q2*q2 + q3*q3);;
			float yaw_cf = atan2f(2.f * (q1*q2 + q0*q3), q0*q0 + q1*q1 - q2*q2 - q3*q3);		//! Yaw, 0 = north, PI/-PI = south, PI/2 = east, -PI/2 = west
			float roll_raw = atan2(-acc[1], -acc[2]) * 180 / 3.14159;
			float pitch_raw = atan2(acc[0], (-acc[2] > 0 ? 1 : -1) * sqrt(acc[1]*acc[1] + acc[2]*acc[2])) * 180 / 3.14159;

			//

			if (time < 9500000)
				continue;

			if (time > 1150000)
				gyro[0] += 1.5 * PI / 180;


			if (!imu_valid && sensor_valid)
			{
				imu_valid = true;

				NonlinearSO3AHRSinit(acc[0], acc[1], acc[2], mag[0], mag[1], mag[2], gyro[0], gyro[1], gyro[2]);
				ekf_est.init(acc[0], acc[1], acc[2], mag[0], mag[1], mag[2], gyro[0], gyro[1], gyro[2]);
			}

			if (imu_valid && sensor_valid && gps_valid && dt < 1)
			{

				// lon/lat to meter
				double lon = gps.longitude / 10000000.0f;
				double lat = gps.latitude / 10000000.0f;
				double latitude_to_meter = 40007000.0f/360;
				double longtitude_to_meter = 40007000.0f/360*cos(lat * PI / 180);
				if (!home_set && gps.DOP[1] < 150)
				{
					home_lon = lon;
					home_lat = lat;
					home_set = true;
				}

				// CF
				float acc_gps_bf[3] = {0};
				float factor = 1.0f;
				float factor_mag = 1.0f;
				NonlinearSO3AHRSupdate(
					acc[0], acc[1], acc[2], mag[0], mag[1], mag[2], gyro[0], gyro[1], gyro[2],
					0.15f*factor, 0.0015f, 0.15f*factor_mag, 0.0015f, dt,
					acc_gps_bf[0], acc_gps_bf[1], acc_gps_bf[2]);

				// experimental EKF
				ahrs.update(acc, gyro, mag, dt);

				// EKF
				EKF_U ekf_u;
				EKF_Mesurement ekf_mesurement;

				ekf_u.accel_x=-acc[0];
				ekf_u.accel_y=-acc[1];
				ekf_u.accel_z=-acc[2];
				ekf_u.gyro_x=gyro[0];
				ekf_u.gyro_y=gyro[1];
				ekf_u.gyro_z=gyro[2];
				ekf_mesurement.Mag_x=-mag[0];
				ekf_mesurement.Mag_y=-mag[1];
				ekf_mesurement.Mag_z=mag[2];

				ekf_mesurement.Pos_Baro_z=quad2.altitude_baro_raw/100.0f;

				if (home_set)
				{
				float yaw_gps = gps.direction * 2 * PI / 360.0f;
				speed_north = cos(yaw_gps) * gps.speed / 100.0f;
				speed_east = sin(yaw_gps) * gps.speed / 100.0f;

				lon_meter = (lon - home_lon) * longtitude_to_meter;
				lat_meter = (lat - home_lat) * latitude_to_meter;
				}

				if(gps_extra.position_accuracy_horizontal < 10 && home_set /*&& time > 500000000*/)
				{
					ekf_mesurement.Pos_GPS_x=lat_meter;
					ekf_mesurement.Pos_GPS_y=lon_meter;
					ekf_mesurement.Vel_GPS_x=speed_north;
					ekf_mesurement.Vel_GPS_y=speed_east;
					ekf_est.set_mesurement_R(0.0005 * gps_extra.position_accuracy_horizontal * gps_extra.position_accuracy_horizontal, 0.02 * gps_extra.velocity_accuracy_horizontal * gps_extra.velocity_accuracy_horizontal);
// 					ekf_est.set_mesurement_R(0.001 * gps_extra.position_accuracy_horizontal * gps_extra.position_accuracy_horizontal, 0.01 * gps_extra.velocity_accuracy_horizontal * gps_extra.velocity_accuracy_horizontal);
// 					ekf_est.set_mesurement_R(1e-3,8e-2);
				}
				else
				{
					ekf_mesurement.Pos_GPS_x=0;
					ekf_mesurement.Pos_GPS_y=0;
					ekf_mesurement.Vel_GPS_x=0;
					ekf_mesurement.Vel_GPS_y=0;
 					ekf_est.set_mesurement_R(1E20,5);
				}

				ekf_est.update(ekf_u,ekf_mesurement, dt);

				devices::gps_data gps_extra2 = gps_extra;

// 					gps_extra2.position_accuracy_horizontal = 15;
				if (time > 183000000 && time < 200000000)
				{
// 					gps_extra2.latitude += (time/1000000.0f - 200) * 5 * (360/40007000.0f);
				}

				if (time > 200000000)
				{
// 					gps_extra2.latitude += 205 * (360/40007000.0f);
// 					gps_extra2.longitude += 205 * (360/40007000.0f);

				}

				memcpy(pos2.gyro, gyro, sizeof(gyro));
				memcpy(&pos2.frame, &frame, sizeof(frame));
				pos2.update(quad5.q, acc, gps_extra2, quad2.altitude_baro_raw/100.0f, dt);
				ins.update(gyro, acc, mag, gps_extra, *(sensors::px4flow_frame*)&frame, quad2.altitude_baro_raw/100.0f, dt, ppm.out[0]>=1240, quad2.airborne);

				if (time > 12000000)
				{

					float P[7];
					for(int p=0; p<7; p++)
						P[p] = ahrs.P[p*8];

					printf("");
				}
			}


			static bool fmt = false;
			if (!fmt)
			{
				fmt = true;

				fprintf(out, "ArduCopter V3.1.5 (3c57e771)\r\n");
				fprintf(out, "Free RAM: 990\r\n");
				fprintf(out, "MoMo\r\n");
				fprintf(out, "FMT, 9, 23, CURR, IhIhh, TimeMS,ThrOut,Volt,Curr,Temp\r\n");
				fprintf(out, "FMT, 9, 23, ATT_OFF_CF, IhIh, TimeMS,Roll,Pitch,Yaw\r\n");
				fprintf(out, "FMT, 9, 23, ATT_ON_CF, IhIh, TimeMS,RollOnCF,PitchOnCF,YawOnCF\r\n");
				fprintf(out, "FMT, 9, 23, ATT_OFF_EKF, IhIh, TimeMS,Roll_EKF,Pitch_EKF,Yaw_EKF\r\n");
				fprintf(out, "FMT, 9, 23, COVAR_OFF_EKF, IhIhhhh, TimeMS,q0,q1,q2,q3,PN,PE\r\n");
				fprintf(out, "FMT, 9, 23, ATT_ON, IhIh, TimeMS,RollOn,PitchOn,YawOn,RollDes,PitchDes,YawDes,RollRate,PitchRate,YawRate,RollRateDes,PitchRateDes,YawRateDes\r\n");
				fprintf(out, "FMT, 9, 23, POSITION, Ihhhhhhhhh, TimeMS,N_GPS,E_GPS,N_EKF,E_EKF,VE_EKF,VE_RAW,POS_ACC,VEL_ACC,HDOP\r\n");
				fprintf(out, "FMT, 9, 23, LatLon, IhIhhh, TimeMS,Lat,Lon,RPOS,RVEL,NSAT,AVGCNO,TOP6CNO\r\n");

				fprintf(out, "FMT, 9, 23, ALT, Ihhhhhhh, TimeMS,BARO,ALT_EKF,ALT_ON, ALT_DST,Sonar,DesSonar,CRate,DesCRate\r\n");
				fprintf(out, "FMT, 9, 23, IMU, Ihhhhhh, TimeMS,ACCX,ACCY,ACCZ,GYROX,GYROY,GYROZ\r\n");
				fprintf(out, "FMT, 9, 23, POS2, Ihhhhhhhhhhhh, TimeMS,POSN,POSE,POSD,VELN,VELE,VELD, abiasx, abiasy, abiasz, vbiasx, vbiasy, vbiasz, sonar_surface, rawn, rawe,flowx,flowy,preX,preY,hx,hy\r\n");
				fprintf(out, "FMT, 9, 23, ON_POS2, Ihhhhhhhhhhhh, TimeMS,POSN,POSE,POSD,VELN,VELE,VELD, abiasx, abiasy, abiasz, vbiasx, vbiasy, vbiasz, rawn, rawe\r\n");
				fprintf(out, "FMT, 9, 23, ACC_NED, Ihhh, TimeMS,ACC_N, ACC_E, ACC_D\r\n");
				fprintf(out, "FMT, 9, 23, AHRS_SEKF, Ihhh, TimeMS,ahrs_roll, ahrs_pitch, ahrs_yaw, ahrs_gyro_bias[0], ahrs_gyro_bias[1], ahrs_gyro_bias[2]\r\n");
				fprintf(out, "FMT, 9, 23, ATT_OFF_INS, Ihhh, TimeMS,ins_roll, ins_pitch, ins_yaw, ins_gyro_bias[0], ins_gyro_bias[1], ins_gyro_bias[2], alt_ins\r\n");
				fprintf(out, "FMT, 9, 23, ATT_RAW, Ihhh, TimeMS, roll_raw, pitch_raw, yaw_raw\r\n");
				fprintf(out, "FMT, 9, 23, P, Ihhhhhhhhh, TimeMS, p[0], p[1], p[2], p[3], p[4], p[5], p[6], pmax, plen\r\n");
				fprintf(out, "FMT, 9, 23, Motor, Ihhhh, TimeMS, m[0], m[1], m[2], m[3]\r\n");
				fprintf(out, "FMT, 9, 23, GPS, Ihhhhh, TimeMS, v[0], v[1], v[2], HDOP, acc_horizontal\r\n");
				fprintf(out, "FMT, 9, 23, VerticalLoop, Ihhhhh, TimeMS,desZAcc,DesCLimb,ZAcc,Climb\r\n");
				fprintf(out, "FMT, 9, 23, PosControl, Ihhhhhhhh, TimeMS,Pos[0],Pos[1],velocity[0],velocity[1],setpoint[0],setpoint[1],velocity_setpoint[0],velocity_setpoint[1],accel_taget[0],accel_taget[1]\r\n");
				fprintf(out, "FMT, 9, 23, POS2_P, Ihhhhhhhh, TimeMS,p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12]\r\n");
				fprintf(out, "FMT, 9, 23, RC, Ihhhhhh, TimeMS, RC[1], RC[2], RC[3], RC[4], RC[5], RC[6]\r\n");
				fprintf(out, "FMT, 9, 23, APP, Ihhhhhh, TimeMS, latency, RC[1], RC[2], RC[3], RC[4]\r\n");
			}


			static int att_id = 0;
			if (att_id++ % 10 == 0 && imu_valid && sensor_valid && gps_valid)
			{
				float euler_sekf[3];
				ahrs.get_euler(euler_sekf);
				fprintf(out, "AHRS_SEKF, %d, %f, %f, %f, %f, %f, %f, %d, %d\r\n", int(time/1000), euler_sekf[0] * 180 / PI, euler_sekf[1] * 180 / PI, euler_sekf[2] * 180 / PI, -ahrs.x[4] * 180 / PI, -ahrs.x[5] * 180 / PI, -ahrs.x[6] * 180 / PI, 0, 0);
				float euler_ins[3];
				ins.get_euler(euler_ins);
				fprintf(out, "ATT_OFF_INS, %d, %f, %f, %f, %f, %f, %f, %f, %d\r\n", int(time/1000), euler_ins[0] * 180 / PI, euler_ins[1] * 180 / PI, euler_ins[2] * 180 / PI, -ahrs.x[4] * 180 / PI, -ahrs.x[5] * 180 / PI, -ahrs.x[6] * 180 / PI, ins.x[9], 0);
				fprintf(out, "ATT_RAW, %d, %f, %f, %f\r\n", int(time/1000), roll_raw, pitch_raw, 0);
				float plen = 0;
				float pmax = 0;
				for(int i=0; i<ahrs.P.m; i++)
				{
					plen += ahrs.P[i*8];
					pmax = max(pmax, ahrs.P[i*8]);
				}
				plen = sqrt(plen);
				pmax = sqrt(pmax);


				fprintf(out, "P, %d, %f, %f, %f, %f, %f, %f, %f, %f, %f\r\n", int(time/1000), sqrt(ahrs.P[0*8]), sqrt(ahrs.P[1*8]), sqrt(ahrs.P[2*8]), sqrt(ahrs.P[3*8]), sqrt(ahrs.P[4*8]), sqrt(ahrs.P[5*8]), sqrt(ahrs.P[6*8]), pmax, plen);

				ekf_est.ekf_result.roll;
				fprintf(out, "ATT_ON, %d, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f\r\n", int(time/1000), quad.angle_pos[0]/100.0f, quad.angle_pos[1]/100.0f, quad.angle_pos[2]/100.0f, quad.angle_target[0]/100.0f, quad.angle_target[1]/100.0f, quad.angle_target[2]/100.0f, quad.speed[0]/100.0f, quad.speed[1]/100.0f, quad.speed[2]/100.0f, quad.speed_target[0]/100.0f, quad.speed_target[1]/100.0f, quad.speed_target[2]/100.0f);
				fprintf(out, "ATT_OFF_CF, %d, %f, %f, %f, %d, %d\r\n", int(time/1000), euler[0] * 180 / PI, euler[1] * 180 / PI, euler[2] * 180 / PI, 0, 0);
				fprintf(out, "ATT_ON_CF, %d, %f, %f, %f, %d, %d\r\n", int(time/1000), roll_cf * 180 / PI, pitch_cf * 180 / PI, yaw_cf * 180 / PI, 0, 0);
				fprintf(out, "ATT_OFF_EKF, %d, %f, %f, %f, %d, %d\r\n", int(time/1000), ekf_est.ekf_result.roll * 180 / PI, ekf_est.ekf_result.pitch * 180 / PI, ekf_est.ekf_result.yaw * 180 / PI, 0, 0);

				fprintf(out, "COVAR_OFF_EKF, %d, %f, %f, %f, %f, %f, %f, %d, %d\r\n", int(time/1000), ekf_est.P[6*14], ekf_est.P[7*14], ekf_est.P[8*14], ekf_est.P[9*14], sqrt(ekf_est.P[0*14]), sqrt(ekf_est.P[4*14]), 0, 0);
				fprintf(out, "IMU, %d, %f, %f, %f, %f, %f, %f\r\n", int(time/1000), acc[0], acc[1], acc[2], gyro[0]*180/PI, gyro[1]*180/PI, gyro[2]*180/PI);
				if(ekf_est.ekf_is_ready())
				{
					fprintf(out, "POSITION, %d, %f, %f, %f, %f, %f, %f, %f, %f, %f, %d, %d\r\n", int(time/1000), lat_meter, lon_meter, ekf_est.ekf_result.Pos_x, ekf_est.ekf_result.Pos_y, ekf_est.ekf_result.Vel_y, speed_east, gps_extra.position_accuracy_horizontal, gps_extra.velocity_accuracy_horizontal, gps.DOP[1]/100.0f, 0, 0);
					fprintf(out, "ALT, %d, %f, %f, %f, %f,%f,%f,%f,%f\r\n", int(time/1000), quad2.altitude_baro_raw/100.0f, ekf_est.ekf_result.Pos_z, quad2.altitude_kalman/100.0f, quad3.altitude_target/100.0f, frame.ground_distance/1000.0f*1.15f, quad4.sonar_target/100.0f, quad2.climb_rate_kalman/100.0f, quad3.climb_target/100.0f);
					fprintf(out, "POS2, %d, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f,%f,%f, %f, %f,%f,%f,%f,%f,%f\r\n", int(time/1000), pos2.x[0], pos2.x[1], pos2.x[2], pos2.x[3], pos2.x[4], pos2.x[5], pos2.x[6], pos2.x[7], pos2.x[8], pos2.x[9], pos2.x[10], pos2.x[11], pos2.x[12], pos2.gps_north, pos2.gps_east, pos2.vx, pos2.vy, pos2.predict_flow[0], pos2.predict_flow[1], pos2.v_hbf[0], pos2.v_hbf[1]);
					fprintf(out, "POS2_P, %d, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f\r\n", int(time/1000), float(pos2.P[0*14]), float(pos2.P[1*14]), float(pos2.P[2*14]), float(pos2.P[3*14]), float(pos2.P[4*14]), float(pos2.P[5*14]), float(pos2.P[6*14]), float(pos2.P[7*14]), float(pos2.P[8*14]), float(pos2.P[9*14]), float(pos2.P[10*14]), float(pos2.P[11*14]), float(pos2.P[12*14]));
					fprintf(out, "ON_POS2, %d, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f,%f,%f\r\n", int(time/1000), on_pos2[0], on_pos2[1], on_pos2[2], on_pos2[3], on_pos2[4], on_pos2[5], on_pos2[6], on_pos2[7], on_pos2[8], on_pos2[9], on_pos2[10], on_pos2[11], pos2.gps_north, pos2.gps_east);
					fprintf(out, "ACC_NED,%d,%f,%f,%f\r\n", int(time/1000), pos2.acc_ned[0], pos2.acc_ned[1], pos2.acc_ned[2]);
				}
				if (home_set)
				fprintf(out, "LatLon, %d, %f, %f, %f, %f, %d, %f, %f\r\n", int(time/1000), gps_extra.latitude, gps_extra.longitude, 0.0005 * gps_extra.position_accuracy_horizontal * gps_extra.position_accuracy_horizontal, 0.02 * gps_extra.velocity_accuracy_horizontal * gps_extra.velocity_accuracy_horizontal, gps.satelite_in_use, avg_cno, top6_cno);
				fprintf(out, "Motor,%d,%d,%d,%d,%d\r\n", int(time/1000), ppm.out[0], ppm.out[1], ppm.out[2], ppm.out[3]);
				fprintf(out, "GPS,%d,%f,%f,%f,%f,%f\r\n", int(time/1000), gps_extra.speed, speed_east, gps_extra.climb_rate, gps_extra.DOP[1]/100.0f, gps_extra.position_accuracy_horizontal);
				fprintf(out, "VerticalLoop,%d,%f,%f,%f,%f\r\n", int(time/1000), (float)quad3.accel_target/100.0f, (float)quad3.climb_target/100.0f, pos2.acc_ned[2], pos2.x[5]);
				fprintf(out, "PosControl,%d,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f\r\n", int(time/1000), posc.pos[0], posc.pos[1], posc.velocity[0], posc.velocity[1], posc.setpoint[0], posc.setpoint[1],posc.velocity_setpoint[0], posc.velocity_setpoint[1], posc.accel_target[0], posc.accel_target[1]);
				fprintf(out, "RC,%d,%d,%d,%d,%d,%d,%d\r\n", int(time/1000), ppm.in[0], ppm.in[1], ppm.in[2], ppm.in[3], ppm.in[4], ppm.in[5]);
				fprintf(out, "APP,%d,%d,%d,%d,%d,%d,%d\r\n", int (time/1000), mobile.latency, mobile.channel[0], mobile.channel[1], mobile.channel[2], mobile.channel[3]);

				if(excel)
				fprintf(excel, "%f,%f,%f\r\n", time/1000000.0f, pos2.x[0], pos2.x[1]);
			}

		}

		else if (tag == TAG_GPS_DATA)
		{
			gps_valid = true;
			memcpy(&gps, data, size);
		}





	}

	fclose(in);
	fclose(out);
	if(excel)
	fclose(excel);

	return 0;
}