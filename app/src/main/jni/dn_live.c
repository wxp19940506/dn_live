#include "com_dongnaoedu_live_jni_PushNative.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/native_window.h>
#define LOGI(FORMAT,...) __android_log_print(ANDROID_LOG_INFO,"jason",FORMAT,##__VA_ARGS__)
#define LOGE(FORMAT,...) __android_log_print(ANDROID_LOG_ERROR,"jason",FORMAT,##__VA_ARGS__)

#include <pthread.h>
#include "queue.h"
#include "x264.h"
#include "rtmp.h"

#ifndef TRUE
#define TRUE	1
#define FALSE	0
#endif

//x264编码输入图像YUV420P
x264_picture_t pic_in;
x264_picture_t pic_out;
//YUV个数
int y_len, u_len, v_len;
//x264编码处理器
x264_t *video_encode_handle;

unsigned int start_time;
//线程处理
pthread_mutex_t mutex;
pthread_cond_t cond;
//rtmp流媒体地址
char *rtmp_path;
//是否直播
int is_pushing = FALSE;

/**
 * 从队列中不断拉取RTMPPacket发送给流媒体服务器）
 */
void *push_thread(void * arg){
	//建立RTMP连接
	RTMP *rtmp = RTMP_Alloc();
	if(!rtmp){
		LOGE("rtmp初始化失败");
		goto end;
	}
	RTMP_Init(rtmp);
	rtmp->Link.timeout = 5; //连接超时的时间
	//设置流媒体地址
	RTMP_SetupURL(rtmp,rtmp_path);
	//发布rtmp数据流
	RTMP_EnableWrite(rtmp);
	//建立连接
	if(!RTMP_Connect(rtmp,NULL)){
		LOGE("%s","RTMP 连接失败");
		goto end;
	}
	//计时
	start_time = RTMP_GetTime();
	if(!RTMP_ConnectStream(rtmp,0)){ //连接流
		LOGE("%s","RTMP ConnectStream failed");
		goto end;
	}
	is_pushing = TRUE;
	while(is_pushing){
		//发送
		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&cond,&mutex);
		//取出队列中的RTMPPacket
		RTMPPacket *packet = queue_get_first();
		if(packet){
			queue_delete_first(); //移除
			packet->m_nInfoField2 = rtmp->m_stream_id; //RTMP协议，stream_id数据
			int i = RTMP_SendPacket(rtmp,packet,TRUE); //TRUE放入librtmp队列中，并不是立即发送
			if(!i){
				LOGE("RTMP 断开");
				RTMPPacket_Free(packet);
				pthread_mutex_unlock(&mutex);
				goto end;
			}else{
				LOGI("%s","rtmp send packet");
			}
			RTMPPacket_Free(packet);
		}

		pthread_mutex_unlock(&mutex);
	}
end:
	LOGI("%s","释放资源");
	free(rtmp_path);
	RTMP_Close(rtmp);
	RTMP_Free(rtmp);
	return 0;
}

JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_startPush
  (JNIEnv *env, jobject jobj, jstring url_jstr){
	//初始化的操作
	const char* url_cstr = (*env)->GetStringUTFChars(env,url_jstr,NULL);
	//复制url_cstr内容到rtmp_path
	rtmp_path = malloc(strlen(url_cstr) + 1);
	memset(rtmp_path,0,strlen(url_cstr) + 1);
	memcpy(rtmp_path,url_cstr,strlen(url_cstr));

	//初始化互斥锁与条件变量
	pthread_mutex_init(&mutex,NULL);
	pthread_cond_init(&cond,NULL);

	//创建队列
	create_queue();
	//启动消费者线程（从队列中不断拉取RTMPPacket发送给流媒体服务器）
	pthread_t push_thread_id;
	pthread_create(&push_thread_id, NULL,push_thread, NULL);

	(*env)->ReleaseStringUTFChars(env,url_jstr,url_cstr);
}


JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_stopPush
  (JNIEnv *env, jobject jobj){
	is_pushing = FALSE;
}


JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_release
  (JNIEnv *env, jobject jobj){

}

/**
 * 设置视频参数
 */
JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_setVideoOptions
  (JNIEnv *env, jobject jobj, jint width, jint height, jint bitrate, jint fps){
	x264_param_t param;
	//x264_param_default_preset 设置
	x264_param_default_preset(&param,"ultrafast","zerolatency");
	//编码输入的像素格式YUV420P
	param.i_csp = X264_CSP_I420;
	param.i_width  = width;
	param.i_height = height;

	y_len = width * height;
	u_len = y_len / 4;
	v_len = u_len;

	//参数i_rc_method表示码率控制，CQP(恒定质量)，CRF(恒定码率)，ABR(平均码率)
	//恒定码率，会尽量控制在固定码率
	param.rc.i_rc_method = X264_RC_CRF;
	param.rc.i_bitrate = bitrate / 1000; //* 码率(比特率,单位Kbps)
	param.rc.i_vbv_max_bitrate = bitrate / 1000 * 1.2; //瞬时最大码率

	//码率控制不通过timebase和timestamp，而是fps
	param.b_vfr_input = 0;
	param.i_fps_num = fps; //* 帧率分子
	param.i_fps_den = 1; //* 帧率分母
	param.i_timebase_den = param.i_fps_num;
	param.i_timebase_num = param.i_fps_den;
	param.i_threads = 1;//并行编码线程数量，0默认为多线程

	//是否把SPS和PPS放入每一个关键帧
	//SPS Sequence Parameter Set 序列参数集，PPS Picture Parameter Set 图像参数集
	//为了提高图像的纠错能力
	param.b_repeat_headers = 1;
	//设置Level级别
	param.i_level_idc = 51;
	//设置Profile档次
	//baseline级别，没有B帧
	x264_param_apply_profile(&param,"baseline");

	//x264_picture_t（输入图像）初始化
	x264_picture_alloc(&pic_in, param.i_csp, param.i_width, param.i_height);
	pic_in.i_pts = 0;
	//打开编码器
	video_encode_handle = x264_encoder_open(&param);
	if(video_encode_handle){
		LOGI("打开编码器成功...");
	}
}

JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_setAudioOptions
  (JNIEnv *env, jobject jobj, jint sampleRateInHz, jint channel){

}

/**
 * 加入RTMPPacket队列，等待发送线程发送
 */
void add_rtmp_packet(RTMPPacket *packet){
	pthread_mutex_lock(&mutex);
	if(is_pushing){
		queue_append_last(packet);
	}
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
}


/**
 * 发送h264 SPS与PPS参数集
 */
void add_264_sequence_header(unsigned char* pps,unsigned char* sps,int pps_len,int sps_len){
	int body_size = 16 + sps_len + pps_len; //按照H264标准配置SPS和PPS，共使用了16字节
	RTMPPacket *packet = malloc(sizeof(RTMPPacket));
	//RTMPPacket初始化
	RTMPPacket_Alloc(packet,body_size);
	RTMPPacket_Reset(packet);

	unsigned char * body = packet->m_body;
	int i = 0;
	//二进制表示：00010111
	body[i++] = 0x17;//VideoHeaderTag:FrameType(1=key frame)+CodecID(7=AVC)
	body[i++] = 0x00;//AVCPacketType = 0表示设置AVCDecoderConfigurationRecord
	//composition time 0x000000 24bit ?
	body[i++] = 0x00;
	body[i++] = 0x00;
	body[i++] = 0x00;

	/*AVCDecoderConfigurationRecord*/
	body[i++] = 0x01;//configurationVersion，版本为1
	body[i++] = sps[1];//AVCProfileIndication
	body[i++] = sps[2];//profile_compatibility
	body[i++] = sps[3];//AVCLevelIndication
	//?
	body[i++] = 0xFF;//lengthSizeMinusOne,H264 视频中 NALU的长度，计算方法是 1 + (lengthSizeMinusOne & 3),实际测试时发现总为FF，计算结果为4.

	/*sps*/
	body[i++] = 0xE1;//numOfSequenceParameterSets:SPS的个数，计算方法是 numOfSequenceParameterSets & 0x1F,实际测试时发现总为E1，计算结果为1.
	body[i++] = (sps_len >> 8) & 0xff;//sequenceParameterSetLength:SPS的长度
	body[i++] = sps_len & 0xff;//sequenceParameterSetNALUnits
	memcpy(&body[i], sps, sps_len);
	i += sps_len;

	/*pps*/
	body[i++] = 0x01;//numOfPictureParameterSets:PPS 的个数,计算方法是 numOfPictureParameterSets & 0x1F,实际测试时发现总为E1，计算结果为1.
	body[i++] = (pps_len >> 8) & 0xff;//pictureParameterSetLength:PPS的长度
	body[i++] = (pps_len) & 0xff;//PPS
	memcpy(&body[i], pps, pps_len);
	i += pps_len;

	//Message Type，RTMP_PACKET_TYPE_VIDEO：0x09
	packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
	//Payload Length
	packet->m_nBodySize = body_size;
	//Time Stamp：4字节
	//记录了每一个tag相对于第一个tag（File Header）的相对时间。
	//以毫秒为单位。而File Header的time stamp永远为0。
	packet->m_nTimeStamp = 0;
	packet->m_hasAbsTimestamp = 0;
	packet->m_nChannel = 0x04; //Channel ID，Audio和Vidio通道
	packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM; //?
	//将RTMPPacket加入队列
	add_rtmp_packet(packet);

}

/**
 * 发送h264帧信息
 */
void add_264_body(unsigned char *buf ,int len){
	//去掉起始码(界定符)
	if(buf[2] == 0x00){  //00 00 00 01
		buf += 4;
		len -= 4;
	}else if(buf[2] == 0x01){ // 00 00 01
		buf += 3;
		len -= 3;
	}
	int body_size = len + 9;
	RTMPPacket *packet = malloc(sizeof(RTMPPacket));
	RTMPPacket_Alloc(packet,body_size);

	unsigned char * body = packet->m_body;
	//当NAL头信息中，type（5位）等于5，说明这是关键帧NAL单元
	//buf[0] NAL Header与运算，获取type，根据type判断关键帧和普通帧
	//00000101 & 00011111(0x1f) = 00000101
	int type = buf[0] & 0x1f;
	//Inter Frame 帧间压缩
	body[0] = 0x27;//VideoHeaderTag:FrameType(2=Inter Frame)+CodecID(7=AVC)
	//IDR I帧图像
	if (type == NAL_SLICE_IDR) {
		body[0] = 0x17;//VideoHeaderTag:FrameType(1=key frame)+CodecID(7=AVC)
	}
	//AVCPacketType = 1
	body[1] = 0x01; /*nal unit,NALUs（AVCPacketType == 1)*/
	body[2] = 0x00; //composition time 0x000000 24bit
	body[3] = 0x00;
	body[4] = 0x00;

	//写入NALU信息，右移8位，一个字节的读取？
	body[5] = (len >> 24) & 0xff;
	body[6] = (len >> 16) & 0xff;
	body[7] = (len >> 8) & 0xff;
	body[8] = (len) & 0xff;

	/*copy data*/
	memcpy(&body[9], buf, len);

	packet->m_hasAbsTimestamp = 0;
	packet->m_nBodySize = body_size;
	packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;//当前packet的类型：Video
	packet->m_nChannel = 0x04;
	packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
//	packet->m_nTimeStamp = -1;
	packet->m_nTimeStamp = RTMP_GetTime() - start_time;//记录了每一个tag相对于第一个tag（File Header）的相对时间
	add_rtmp_packet(packet);

}

/**
 * 将采集到视频数据进行编码
 */
JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_fireVideo
  (JNIEnv *env, jobject jobj, jbyteArray buffer){
	//视频数据转为YUV420P
	//NV21->YUV420P
	jbyte* nv21_buffer = (*env)->GetByteArrayElements(env,buffer,NULL);
	jbyte* u = pic_in.img.plane[1];
	jbyte* v = pic_in.img.plane[2];
	//nv21 4:2:0 Formats, 12 Bits per Pixel
	//nv21与yuv420p，y个数一致，uv位置对调
	//nv21转yuv420p  y = w*h,u/v=w*h/4
	//nv21 = yvu yuv420p=yuv y=y u=y+1+1 v=y+1
	memcpy(pic_in.img.plane[0], nv21_buffer, y_len);
	int i;
	for (i = 0; i < u_len; i++) {
		*(u + i) = *(nv21_buffer + y_len + i * 2 + 1);
		*(v + i) = *(nv21_buffer + y_len + i * 2);
	}

	//h264编码得到NALU数组
	x264_nal_t *nal = NULL; //NAL
	int n_nal = -1; //NALU的个数
	//进行h264编码
	if(x264_encoder_encode(video_encode_handle,&nal, &n_nal,&pic_in,&pic_out) < 0){
		LOGE("%s","编码失败");
		return;
	}
	//使用rtmp协议将h264编码的视频数据发送给流媒体服务器
	//帧分为关键帧和普通帧，为了提高画面的纠错率，关键帧应包含SPS和PPS数据
	int sps_len , pps_len;
	unsigned char sps[100];
	unsigned char pps[100];
	memset(sps,0,100);
	memset(pps,0,100);
	pic_in.i_pts += 1; //顺序累加
	//遍历NALU数组，根据NALU的类型判断
	for(i=0; i < n_nal; i++){
		if(nal[i].i_type == NAL_SPS){
			//复制SPS数据
			sps_len = nal[i].i_payload - 4;
			memcpy(sps,nal[i].p_payload + 4,sps_len); //不复制四字节起始码
		}else if(nal[i].i_type == NAL_PPS){
			//复制PPS数据
			pps_len = nal[i].i_payload - 4;
			memcpy(pps,nal[i].p_payload + 4,pps_len); //不复制四字节起始码

			//发送序列信息
			//h264关键帧会包含SPS和PPS数据
			add_264_sequence_header(pps,sps,pps_len,sps_len);

		}else{
			//发送帧信息
			add_264_body(nal[i].p_payload,nal[i].i_payload);
		}

	}
}


JNIEXPORT void JNICALL Java_com_dongnaoedu_live_jni_PushNative_fireAudio
  (JNIEnv *env, jobject jobj, jbyteArray array, jint len){

}
