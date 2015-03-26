#pragma once
#include <stdint.h>

namespace devices
{
	typedef struct
	{
		float x;				// unit: meter/s
		float y;
		float z;
		float temperature;		// unit: degree Celsius // use NAN if no temperature available.
	} accelerometer_data;

	class IAccelerometer
	{
		// return 0 if new data available, 1 if old data, negative for error.
		int read(accelerometer_data *out);
	};
}