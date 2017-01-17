#include "camera.h"
#include "encoder.h"
#include <libyuv.h>

using namespace sensors;
using namespace devices;

static int64_t getus()
{    
   struct timespec tv;  
   clock_gettime(CLOCK_MONOTONIC, &tv);    
   return (int64_t)tv.tv_sec * 1000000 + tv.tv_nsec/1000;    
}


int camera_init()
{
	RK3288Camera51 c;
	c.init(0);

	uint8_t *p = NULL;

	printf("camera start\n");

	uint8_t * nv12 = new uint8_t[640*480*3/2];
	uint8_t * yv12 = new uint8_t[640*480*3/2];

	int64_t v = getus();
	for(int i=0; i<50; i++);
	libyuv::NV12ToI420(nv12, 640, nv12+640*480, 640, yv12, 640, yv12+640*480, 320, yv12+640*480*5/4, 320, 640, 480);
	v = getus() - v;
	printf("nv12 to i420: %d\n", int(v));
	
	delete [] nv12;
	delete [] yv12;

	frame_format fmt;
	c.get_frame_format(&fmt);
	printf("fmt:%dx%d\n", fmt.width, fmt.height);

	v = -1;
	int frame_count = 0;

	uint8_t *y = new uint8_t[1920*1080];
	uint8_t *uv = new uint8_t[960*540*2];
	uint8_t *yuv360 = new uint8_t[640*360*3/2];

	android_video_encoder enc;
	enc.init(640, 360, 250000);
	FILE * f = fopen("/data/enc.h264", "wb");

	printf("live streaming encoder\n");

	/*
	android_video_encoder enc2;
	enc2.init(1920, 1080, 3000000);
	FILE * f2 = fopen("/data/hi.h264", "wb");

	printf("hi res encoder\n");

	*/

	// low bandwidth H264 encoder test
	FILE * fyuv = fopen("/data/640.yuv", "rb");
	for(int i=0; i<70; i++)
	{
		// drain encoder
		uint8_t *ooo = NULL;
		int encoded_size = enc.get_encoded_frame(&ooo);
		if (encoded_size > 0 && ooo)
		{
			int nal_type = ooo[4] & 0x1f;

			printf("live streaming: %d, %d\n", encoded_size, nal_type);
			fwrite(ooo, 1, encoded_size, f);
			fflush(f);
		}

		// feed live streaming encoder
		void *live = enc.get_next_input_frame_pointer();
		if (live)
		{
			fread(live, 1, 640*360*3/2, fyuv);
			enc.encode_next_frame();
		}
	}

	fflush(f);
	fclose(f);
	exit(1);

	while(1)
	{
		// drain live streaming
		uint8_t *ooo = NULL;
		int encoded_size = enc.get_encoded_frame(&ooo);
		if (encoded_size > 0 && ooo)
		{
			int nal_type = ooo[4] & 0x1f;

			printf("live streaming: %d, %d\n", encoded_size, nal_type);
			fwrite(ooo, 1, encoded_size, f);
			fflush(f);
		}

		/*

		// drain hi res
		ooo = NULL;
		encoded_size = enc2.get_encoded_frame(&ooo);
		if (encoded_size > 0 && ooo)
		{
			printf("hi res %d\n", encoded_size);
			fwrite(ooo, 1, encoded_size, f2);
			fflush(f2);
		}

		*/

		// capture new frames
		int s = c.get_frame(&p);
		if (s == 0)
		{
			// got frame, copy it out
			if (v == -1)
				v = getus()-1;

			printf("frame:%d, %dfps\n", frame_count, int64_t(frame_count)*1000000/(getus()-v));
			frame_count++;

			c.release_frame(p);
			/*
			memcpy(y, p, 1920*1080);

			libyuv::SplitUVPlane(p + 1920*1080, 1920, uv, 960, uv + 1920*1080/4, 960, 960, 540);

			// scale down for live streaming
			int64_t t = getus();
			libyuv::I420Scale(y, 1920, uv, 960, uv + 1920*1080/4, 960, 1920, 1080,
				yuv360, 640, yuv360 + 640*360, 320, yuv360 + 640*360*5/4, 320, 640, 360, libyuv::kFilterBilinear);
			t = getus() - t;
			//printf("scale=%d\n", int(t));
			*/

			// feed live streaming encoder
			void *live = enc.get_next_input_frame_pointer();
			if (live)
			{
				memcpy(live, p, 640*360*3/2);
				enc.encode_next_frame();
			}

			// feed high res encoder
			/*
			void *hi = enc2.get_next_input_frame_pointer();
			if (hi)
			{
				memcpy(hi, y, 1920*1080);
				memcpy((uint8_t*)hi+1920*1080, uv, 1920*1080/2);
				enc2.encode_next_frame();
			}
			*/
		}

		else
		{
			usleep(10000);
		}

	}

	return 0;
}