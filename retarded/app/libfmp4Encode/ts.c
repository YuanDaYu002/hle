 
/***************************************************************************
* @file: ts.c
* @author:   
* @date:  4,15,2019
* @brief:  H264 + AAC 编码成 TS 文件
* @attention:
***************************************************************************/
#include <sys/time.h>
#include <pthread.h>
#include <stdlib.h>


#include "ts.h"
#include "ts_audio.h"
#include "ts_video.h"
#include "ts_interface.h"
#include "ts_print.h"
#include "buf.h"
#include "bitstream.h"
#include "crc.h"
#include "my_inet.h"

#define PID_PMT		0x0100
#define TRANSPORT_STREAM_ID 0x0001
#define PROGRAM_NUMBER 0x0001

#define AUDIO_stream_PID	1
#define VIDEO_stream_PID	2

#define VIDEO_INDEX  0  //媒体信息数组下标
#define AUDIO_INDEX  1
#define LEAD_TRACK  VIDEO_INDEX  

#define MAX_AUDIO_FRAME  1000    //最大容许接收的audio帧数（AAC：16000/1024 = 16帧/s ; 1000/16 = 62s）
#define MAX_VIDEO_FRAME  1000    //最大容许接收的video帧数（h264：15帧/s ; 1000/15=66s）

extern void print_array(unsigned char* box_name,unsigned char*start,unsigned int length);


extern ts_video_init_t ts_video_init_info;
extern ts_audio_init_t ts_audio_init_info;


/*******************************************************************************
*@ Description    :初始化 TS 的 header 部分(固定4字节)
*@ Input          :<out_buf>输出buf
					<out_buf_size>输出buf的大小
					<cont_count>递增计数器，从0-f，起始值不一定取0，但必须是连续的
					<payload_unit_start>负载单元起始标示符，一个完整的数据包开始时标记为1
					<need_pcr>
					<fpcr>
					<pid>pid值
					<discontiniuty>
					<payload_size> 头部后边还要附加的数据长度（ts包大小固定为188字节）
*@ Output         :
*@ Return         :header 的字节数
*@ attention      :
*******************************************************************************/
static int generate_ts_header(char* out_buf, int out_buf_size, int cont_count, int payload_unit_start, 
								int need_pcr, double fpcr, int pid, int discontiniuty, int payload_size)
{
	PutBitContext bs;
	int pos;
	long long pcr;
	int stuffing_size = 0;
	int i;
	/*是否包含自适应区:
		‘00’保留；
		‘01’为无自适应域，仅含有效负载；
		‘10’为仅含自适应域，无有效负载；
		‘11’为同时带有自适应域和有效负载。
	*/
	int adaptation_field = 0;

	if (payload_size < 184 || need_pcr || discontiniuty)//说明有自适应区域占了空间
	{
		adaptation_field = 3;
	}
	else // >= 184 说明无自适应域
	{
		adaptation_field = 1;
	}
	
	if (payload_size == 0)
		adaptation_field = 2;

	/*---#TS header 4字节------------------------------------------------------------*/
	init_put_bits(&bs, out_buf, out_buf_size);
	put_bits(&bs, 8, 0x47); //sync byte
	//put_bits(&bs, 4, 0x7); //sync byte
	put_bits(&bs, 1, 0x00); //transport_error_indicator
	put_bits(&bs, 1, payload_unit_start);//payload_unit_start_indicator
	put_bits(&bs, 1, 0x00); //transport pget_frame_durationriority
	put_bits(&bs, 13, pid ); //pid
	put_bits(&bs, 2, 0x00); //transport_scrambling_control
	put_bits(&bs, 2, adaptation_field); //adaptation_field_control
	put_bits(&bs, 4, cont_count & 0x0F); //continiuty counter

	if (adaptation_field == 2 || adaptation_field == 3)//包含自适应区域
	{
		int adaptation_field_length = 0;
		if (discontiniuty || need_pcr || payload_size < 184 - adaptation_field_length)
			++adaptation_field_length;
		if (need_pcr)
			adaptation_field_length += 6;
		if (payload_size < 184 - adaptation_field_length)
		{
			stuffing_size = 184 - 1 - adaptation_field_length - payload_size;
			adaptation_field_length += stuffing_size;
		}

		put_bits(&bs, 8, adaptation_field_length);//adaptation field length
		///TODO:need to calcuate
		if (adaptation_field_length > 0)
		{
			put_bits(&bs, 1, discontiniuty);//discontinuity indicator
			put_bits(&bs, 1, 0);//random access indicator
			put_bits(&bs, 1, 0);//ES priority indicator
			put_bits(&bs, 1, need_pcr);//PCR flag
			put_bits(&bs, 1,0x00);//OPCR flag
			put_bits(&bs, 1,0x00);//splicing point flag
			put_bits(&bs, 1,0x00);//transport private data flag
			put_bits(&bs, 1,0x00);//adaptation field extention flag
			if (need_pcr)
			{
				pcr = fpcr * 27000000LL;
				put_bits64(&bs, 33,(pcr / 300LL) & 0x01FFFFFFFFLL);//program clock reference base
				put_bits(&bs, 6,0x3F);//reserved
				put_bits(&bs, 9,(pcr % 300) & 0x01FFLL);//program clock reference ext
			}
			//here we have to put stuffing byte
			for(i = 0; i < stuffing_size; i++)
			{
				put_bits(&bs, 8, 0xFF);
			}
		}
	}

	flush_put_bits(&bs);
	return put_bits_count(&bs)/8;
}

/*******************************************************************************
*@ Description    :生成 pes 头
*@ Input          :<out_buf> 生成头存储的buf
					<out_buf_size>生成头存储的buf大小
					<data_size>数据部分的大小
					<fpts>
					<fdts>
					<es_id>音频取值（0xc0-0xdf），通常为0xc0
							视频取值（0xe0-0xef），通常为0xe0
					
*@ Output         :
*@ Return         :返回字节长度
*@ attention      :
*******************************************************************************/
static int generate_pes_header(char* out_buf, int out_buf_size, int data_size, 
									double fpts, double fdts, int es_id)
{
	PutBitContext bs;

	init_put_bits(&bs, out_buf, out_buf_size);
	int pts_dts_length = 5;
	long long pts;
	long long dts;

	pts = (fpts + 0.300) * 90000LL;
	dts = (fdts + 0.300) * 90000LL;

	if (abs(pts-dts) > 10)
		pts_dts_length += 5;

	/*---# pes packet start------------------------------------------------------------*/
	put_bits(&bs, 24,0x0000001);// pes packet start code

	put_bits(&bs, 8,	es_id);// stream id 音频取值（0xc0-0xdf），通常为0xc0 视频取值（0xe0-0xef），通常为0xe0
	if (es_id >= 0xE0)
		put_bits(&bs, 16,	0);	// pes packet length 后面pes数据的长度，0表示长度不限制，只有视频数据长度会超过0xffff
	else
		put_bits(&bs, 16,	data_size + 3 + pts_dts_length);// pes packet length

	/*---#第一个 flag（1Byte） 分解------------------------------------------------------------*/
	put_bits(&bs, 2, 0x02);			// have to be '10b'
	put_bits(&bs, 2, 0x00);			// pes scrambling control
	put_bits(&bs, 1, 0x00);			// pes priority
	put_bits(&bs, 1, 0x01);			// data alignment
	put_bits(&bs, 1, 0x00);			// copyright
	put_bits(&bs, 1, 0x00);			// original or copy
	
	/*---#第二个 flag（1Byte） 分解------------------------------------------------------------*/
	if (pts_dts_length > 5){
		put_bits(&bs, 2, 0x03);		// pts/dts flags we have only pts
	}else{
		put_bits(&bs, 2, 0x02);
	}
	put_bits(&bs, 1, 0x00);			// escr flag
	put_bits(&bs, 1, 0x00);			// es rate flag
	put_bits(&bs, 1, 0x00);			// dsm trick mode flag
	put_bits(&bs, 1, 0x00);			// additional copy info flag
	put_bits(&bs, 1, 0x00);			// pes crc flag
	put_bits(&bs, 1, 0x00);			// pes extention flag
	
	put_bits(&bs, 8, pts_dts_length);// pes headder data length（pes data length）后面数据的长度，取值5或10

	/*---# 填入 pts（5B） + dts（5B） 数据------------------------------------------------------------*/
	if (pts_dts_length > 5)
	{
		put_bits(&bs, 4,  0x03);// have to be '0011b'
		put_bits(&bs, 3,  (pts >> 30) & 0x7);// pts[32..30]
		put_bits(&bs, 1,  0x01);// marker bit
		put_bits(&bs, 15, (pts >> 15) & 0x7FFF);// pts[29..15]
		put_bits(&bs, 1,  0x01);// marker bit
		put_bits(&bs, 15, pts & 0x7FFF);// pts[14..0]
		put_bits(&bs, 1,  0x01);// marker bit

		put_bits(&bs, 4,  0x01);// have to be '0011b'
		put_bits(&bs, 3,  (dts >> 30) & 0x7);// pts[32..30]
		put_bits(&bs, 1,  0x01);// marker bit
		put_bits(&bs, 15, (dts >> 15) & 0x7FFF);// pts[29..15]
		put_bits(&bs, 1,  0x01);// marker bit
		put_bits(&bs, 15, dts & 0x7FFF);// pts[14..0]
		put_bits(&bs, 1,  0x01);// marker bit

	}
	else
	{
		put_bits(&bs, 4,  0x02);// have to be '0010b'

		put_bits(&bs, 3,  (pts >> 30) & 0x7);// pts[32..30]
		put_bits(&bs, 1,  0x01);// marker bit
		put_bits(&bs, 15, (pts >> 15) & 0x7FFF);// pts[29..15]
		put_bits(&bs, 1,  0x01);// marker bit
		put_bits(&bs, 15, pts & 0x7FFF);// pts[14..0]
		put_bits(&bs, 1,  0x01);// marker bit
	}


	flush_put_bits(&bs);
	return put_bits_count(&bs)/8;

}


/*******************************************************************************
*@ Description    :对一块源帧数据（可为多帧）进行 TS 打包
*@ Input          :<ts_buf>TS文件的输出buf
					<frame_count>打包成TS包的包（帧）总数（ts包大小固定为188字节）
					<cc>TS包的包（帧）总数计数（上层）
					<pts>
					<dts>
					<es_id> video track：0xE0;  否则：0xC0
					<pid>
					<data>源帧数据
					<frame_size>源帧数据的总大小
					<pcr_pid> video track（lead track）：1 否则:0 
					
*@ Output         :
*@ Return         :
*@ attention      :
*******************************************************************************/
void pack_data(char* ts_buf, int* frame_count, int* cc, double pts, double dts,
					int es_id, int pid, char* data, int frame_size, int pcr_pid)
{
	char pes_header_buffer[128];
	char ts_header_buffer[188];

	int pes_header_size = 0;
	int ts_header_size;
	int pos = 0;
	if (frame_size > 0){
		pes_header_size = generate_pes_header(pes_header_buffer, sizeof(pes_header_buffer), frame_size, pts, dts, es_id);
	}
	pos = 0;

	if ( pcr_pid){
		ts_header_size = generate_ts_header(ts_header_buffer, sizeof(ts_header_buffer), cc[0], 1,
												1, dts, pid, 0, frame_size + pes_header_size);
	}else{
		ts_header_size = generate_ts_header(ts_header_buffer, sizeof(ts_header_buffer), cc[0], 1,
												0, 0, pid, 0, frame_size + pes_header_size);
	}
	/*---# ts 层------------------------------------------------------------*/
	memcpy(ts_buf + 188 * frame_count[0], ts_header_buffer, ts_header_size);
	/*---# pes 层------------------------------------------------------------*/
	memcpy(ts_buf + 188 * frame_count[0] + ts_header_size, pes_header_buffer, pes_header_size);
	/*---# es 层------------------------------------------------------------*/
	//每个TS包长188字节，剩余的部分写帧数据
	memcpy(ts_buf + 188 * frame_count[0] + ts_header_size + pes_header_size, data, 188 - ts_header_size - pes_header_size);

	++frame_count[0];
	++cc[0];

	pos += 188 - ts_header_size - pes_header_size;

	while (pos < frame_size)
	{

	//	ts_header_size = generate_ts_header(ts_header_buffer, sizeof(ts_header_buffer), cc[0],
	//										0, 1, start_time + pos / 8000.0, pid, 0, frame_size - pos);
		ts_header_size = generate_ts_header(ts_header_buffer, sizeof(ts_header_buffer), cc[0],
													0, 0, 0, pid, 0, frame_size - pos);

		memcpy(ts_buf + 188 * frame_count[0], ts_header_buffer, ts_header_size);
		memcpy(ts_buf + 188 * frame_count[0] + ts_header_size , data + pos, 188 - ts_header_size);

		pos += 188 - ts_header_size;
		++frame_count[0];
		++cc[0];
	}
}

					


/*******************************************************************************
*@ Description    :放入TS文件 TS层中的 PAT 表
*@ Input          :
*@ Output         :
*@ Return         :188（TS包的固定大小）
*@ attention      :
*******************************************************************************/
int TS_put_pat(char* buf, int* pat_cc)
{
	PutBitContext bs;
	int pos;
	long long pcr;
	int stuffing_size;
	int i;
	unsigned char tmp[188] = {0xFF};

	int pat_size = 0;
	int header_size = 0;

	init_put_bits(&bs, tmp, 188);
	put_bits(&bs, 8, 0);//table offset
	put_bits(&bs, 8, 0);//table id PAT

	put_bits(&bs, 1, 0x01);//section syntax indicator
	put_bits(&bs, 1, 0);//have to be 0
	put_bits(&bs, 2, 0x03);//reserved
	put_bits(&bs, 12, (16 + 2 + 5 + 1 + 8 + 8 + 16 + 3 + 13 + 32)/8);//section length
	put_bits(&bs, 16, TRANSPORT_STREAM_ID);//transport stream id
	put_bits(&bs, 2, 0x03);//reserved
	put_bits(&bs, 5, 0x00);//version number
	put_bits(&bs, 1, 0x01);//current next indicator
	put_bits(&bs, 8, 0x00);//section number
	put_bits(&bs, 8, 0x00);//last section number


		put_bits(&bs, 16, PROGRAM_NUMBER);//program number
		put_bits(&bs, 3, 0x07);//reserved
		put_bits(&bs, 13, PID_PMT);//program map id

	put_bits(&bs, 32, t_htonl (crc(crc_get_table(CRC_32_IEEE), -1, (const uint8_t*)(&tmp[0]) + 1,  put_bits_count(&bs)/8 - 1) ));

	pat_size = put_bits_count(&bs)/8;
	memset(&tmp[pat_size], 0xff, 188-pat_size);//不够的长度直接补0xff

	header_size = generate_ts_header(buf, 188, pat_cc[0], 1, 0, 0, 0, 0, 184);
	memcpy(buf + header_size, tmp, 184);
	++pat_cc[0];
	return 188;
}


/*******************************************************************************
*@ Description    : 放入TS文件 TS层中的 PMT 表
*@ Input          :
					<track>音视频轨道信息（一般为2，音视频各一条）
					<pmt_cc> pmt的数量统计
					<pcr_pid> PCR(节目参考时钟)所在TS分组的PID，指定为视频PID
*@ Output         :<buf>结果输出
*@ Return         :188（TS包的固定大小）
*@ attention      :
*******************************************************************************/
int TS_put_pmt(char* buf, ts_media_stats_t* status, int* pmt_cc, int pcr_pid)
{
	PutBitContext bs;
	int pos;
	long long pcr;
	int stuffing_size;
	int i;
	unsigned char tmp[188];
	int pmt_size = 0;
	int header_size = 0;

	init_put_bits(&bs, tmp, 188);

	//	CBitstream bs;
	//	bs.Open(result->ptr);
	put_bits(&bs, 8, 0);//table offset
	put_bits(&bs, 8, 0x02);//table id for PMT
	put_bits(&bs, 1, 0x01);//section syntax indicator
	put_bits(&bs, 1, 0x00);//0
	put_bits(&bs, 2, 0x03);//reserved
	put_bits(&bs, 12, (16 + 2 + 5 + 1 + 8 + 8 + 16 + 16 + (8 + 16 + 16)*status->n_tracks + 32)/8);//section length
	put_bits(&bs, 16, PROGRAM_NUMBER);//program number
	put_bits(&bs, 2, 0x03);//reserved
	put_bits(&bs, 5, 0x00);//version number
	put_bits(&bs, 1, 0x01);//current next indicator
	put_bits(&bs, 8, 0x00);//section number
	put_bits(&bs, 8, 0x00);//last section number

	put_bits(&bs, 3, 0x07);//reserved
	put_bits(&bs, 13, PID_PMT + pcr_pid);//PCR PID  PCR(节目参考时钟)所在TS分组的PID，指定为视频PID

	put_bits(&bs, 4, 0x0F);//reserved
	put_bits(&bs, 12, 0x0000);//program info length
	/*
		编解码类型：0x0F：mp4a（代表该trak为音频trak信息）;
		0x1B：avc1（代表该trak为视频trak信息）;   		0：啥都不是
	*/
	for(i = status->n_tracks - 1; i >= 0; --i)
	{
		put_bits(&bs, 8, status->track[i].codec);//stream type
		put_bits(&bs, 3, 0x07);//reserved
		put_bits(&bs, 13, PID_PMT + status->n_tracks - i);//elementary PID
		put_bits(&bs, 4, 0x0F);//reserved
		put_bits(&bs, 12, 0x0000);//es info lenght
	}

	put_bits(&bs, 32, t_htonl( crc(crc_get_table(CRC_32_IEEE), -1,  (const uint8_t*)(&tmp[0]) + 1,  put_bits_count(&bs)/8 - 1) ));//here we calculate CRC of section without first Pointer byte

	pmt_size = put_bits_count(&bs)/8;

	memset(&tmp[pmt_size], 0xff, 188- pmt_size);//不够的长度直接补0xff

	header_size = generate_ts_header(buf, 188, pmt_cc[0], 1, 0, 0, PID_PMT, 0, 184);
	memcpy(buf + header_size, tmp, 184);
	++pmt_cc[0];
	return 188;
}


/*******************************************************************************
*@ Description    :对源音视频帧数据进行TS打包（188字节一包）
*@ Input          :<buf> 输出buf
					<stats>媒体状态信息
					<data>媒体数据信息
					<track>当前 track 的下标
					<lead_track> lead track 的下标
					<num_of_frames>本次要打包多少帧数据
*@ Output         :
*@ Return         :返回TS打包后的数据总长度
*@ attention      :
*******************************************************************************/
int TS_put_data_frame(char* buf, ts_media_stats_t* stats, ts_media_data_t* data, 
							int track, int lead_track, int num_of_frames)
{

	int fc =  0;//ts（包/帧）的数量统计
	int first_frame = data->track[track].first_frame;//当前TS分片对应帧区间的开始位置（下标）
	int fn = data->track[track].frames_written;//记录已经写入到TS文件（缓冲区）的帧数
	char* data_buf = data->track[track].buffer + data->track[track].offset[fn];//下一个写入的帧数据位置
	//print_array("data_buf:",data_buf,20);
	int data_buf_size = 0;//帧数据总大小
	int i;
	double pts;
	double dts;

	for(i = 0; i < num_of_frames && ((i + fn) < data->track[track].n_frames); ++i)
	{
		data_buf_size += data->track[track].size[fn + i];
	}

	int es_id = 0xC0;

	if (stats->track[track].codec == H264_VIDEO)
		es_id = 0xE0;
	
	pts = stats->track[track].pts[first_frame + fn];
	dts = stats->track[track].dts[first_frame + fn];

	if (stats->track[track].repeat_for_every_segment)//在 lead track 的基础上展开pts和dts
	{
		pts += stats->track[lead_track].pts[data->track[lead_track].first_frame];
		dts += stats->track[lead_track].dts[data->track[lead_track].first_frame];
	}

	pack_data(buf, &fc, &data->track[track].cc, pts, dts, es_id, PID_PMT + stats->n_tracks - track,
			data_buf,data_buf_size, track == lead_track ? 1 : 0);

	data->track[track].frames_written += num_of_frames;

	return fc * 188;//return number of written bytes
}


/*******************************************************************************
*@ Description    :返回当前需要写数据的 track 数组下标
*@ Input          :<stats>媒体音视频轨道信息
					<data>一个TS文件对应从trak中取出帧数据的描述信息
*@ Output         :
*@ Return         :
*@ attention      :
*******************************************************************************/
int select_current_track(ts_media_stats_t* stats, ts_media_data_t* data)
{
	int ct = -1;
	float min_time = 1000000000.0;
	int  i;

	for(i = 0; i < stats->n_tracks; ++i)
	{
		ts_track_data_t* td = &data->track[i]; //当前TS文件 音/视频 trak 对应源数据帧的描述信息 
		ts_track_media_t* ts = &stats->track[i];
		float* pts = ts->dts;//优先设置成pts = dts，如果一个视频没有B帧，则pts永远和dts相同
		if ( !pts )
			pts = ts->pts;


		if (td->frames_written + td->first_frame < ts->n_frames && td->frames_written < td->n_frames)
		{
			if (pts[td->first_frame + td->frames_written] < min_time)
			{
				min_time = pts[td->first_frame + td->frames_written];
				ct = i;
			}
		}
	}
	return ct;
}



/*******************************************************************************
*@ Description    :TS切片编码初始化函数
*@ Input          :
*@ Output         :
*@ Return         :成功：0 失败：-1
*@ attention      :
*******************************************************************************/
#define TS_RECODER_BUF_SIZE  1024*512*4		//TS文件缓存buf大小(最终的TS文件数据)
#define VIDEO_BUF_SIZE		 1024*512*3		//缓存video帧（15S总帧数）的buf大小（1.5M）
#define AUDIO_BUF_SIZE		 1024*512*1		//缓存audio帧（15S总帧数）的buf大小（0.5M）

buf_t ts_recoder_buf = {0} ;		//TS文件缓存buf
static ts_media_stats_t media_stats = {0};	//媒体状态信息描述
static ts_media_data_t  media_data = {0}; 	//媒体数据信息描述
int TS_recoder_init(ts_recoder_init_t *config)
{
	int ret;
	ts_recoder_init_t tmp_config = {0};
	memcpy(&tmp_config,config,sizeof(ts_recoder_init_t));
	ts_global_variable_reset();
	
	ret = init_buf(&ts_recoder_buf,TS_RECODER_BUF_SIZE);
	if(ret < 0){
		TS_ERROR_LOG("init_buf failed !\n");
		return -1;
	}
	
	TS_video_init(&tmp_config.video_config);
	TS_Audio_init(&tmp_config.audio_config);

	/*---#------------------------------------------------------------
			media_stats 初始化
	  ---#------------------------------------------------------------*/
	memset(&media_stats,0,sizeof(media_stats));
	media_stats.n_tracks = 2;	//2条轨道
	/*---# video部分------------------------------------------------------------*/
	media_stats.track[VIDEO_INDEX].codec = 0x1B; // video track
	//media_stats.track[VIDEO_INDEX].elementary_PID = VIDEO_stream_PID;
	media_stats.track[VIDEO_INDEX].n_frames = 0;
	media_stats.track[VIDEO_INDEX].bitrate = 0;//不填
	media_stats.track[VIDEO_INDEX].pts = (float*)calloc(MAX_VIDEO_FRAME*sizeof(float),sizeof(char));
	if(media_stats.track[VIDEO_INDEX].pts == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}
	
	media_stats.track[VIDEO_INDEX].dts = (float*)calloc(MAX_VIDEO_FRAME*sizeof(float),sizeof(char));
	if(media_stats.track[VIDEO_INDEX].dts == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}

	media_stats.track[VIDEO_INDEX].repeat_for_every_segment = 0;
	media_stats.track[VIDEO_INDEX].flags = (int*)calloc(MAX_VIDEO_FRAME*sizeof(int),sizeof(char));
	if(media_stats.track[VIDEO_INDEX].flags == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}

	media_stats.track[VIDEO_INDEX].sample_rate = 90000;
		
	/*---# audio部分------------------------------------------------------------*/
	media_stats.track[AUDIO_INDEX].codec = 0x0F; // audio track
	//media_stats.track[1].elementary_PID = AUDIO_stream_PID;
	media_stats.track[AUDIO_INDEX].n_frames = 0;
	media_stats.track[AUDIO_INDEX].bitrate = 0;
	media_stats.track[AUDIO_INDEX].pts = (float*)calloc(MAX_AUDIO_FRAME*sizeof(float),sizeof(char));
	if(media_stats.track[AUDIO_INDEX].pts == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}

	media_stats.track[AUDIO_INDEX].dts = (float*)calloc(MAX_AUDIO_FRAME*sizeof(float),sizeof(char));
	if(media_stats.track[AUDIO_INDEX].dts == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}

	media_stats.track[AUDIO_INDEX].repeat_for_every_segment = 0;
	media_stats.track[AUDIO_INDEX].flags = (int*)calloc(MAX_AUDIO_FRAME*sizeof(int),sizeof(char));
	if(media_stats.track[AUDIO_INDEX].flags == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}

	media_stats.track[AUDIO_INDEX].sample_rate = config->audio_config.sample_rate;
	media_stats.track[AUDIO_INDEX].n_ch = config->audio_config.n_ch;
	media_stats.track[AUDIO_INDEX].sample_size = 0;
	
	/*---#------------------------------------------------------------
			media_data 初始化
	 ---#------------------------------------------------------------*/
	 memset(&media_data,0,sizeof(media_data));
	media_data.n_tracks = 2;
	/*---# video部分------------------------------------------------------------*/
	media_data.track[VIDEO_INDEX].n_frames = 0;
	media_data.track[VIDEO_INDEX].first_frame = 0;
	media_data.track[VIDEO_INDEX].buffer = (char*)calloc(VIDEO_BUF_SIZE,sizeof(char));  //15s的视频数据缓存
	if(media_data.track[VIDEO_INDEX].buffer == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}
	media_data.track[VIDEO_INDEX].buffer_w_pos = media_data.track[VIDEO_INDEX].buffer;
	media_data.track[VIDEO_INDEX].buffer_r_pos = media_data.track[VIDEO_INDEX].buffer;
	media_data.track[VIDEO_INDEX].buffer_size = VIDEO_BUF_SIZE;
	media_data.track[VIDEO_INDEX].size = (int *)calloc(MAX_VIDEO_FRAME*sizeof(int),sizeof(char));
	if(media_data.track[VIDEO_INDEX].size == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;	
	}

	media_data.track[VIDEO_INDEX].offset = (int *)calloc(MAX_VIDEO_FRAME*sizeof(int),sizeof(char));
	if(media_data.track[VIDEO_INDEX].offset == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;	
	}

	media_data.track[VIDEO_INDEX].frames_written = 0;
	media_data.track[VIDEO_INDEX].data_start_offset = 0;
	media_data.track[VIDEO_INDEX].cc = 0;
	
	/*---# audio 部分------------------------------------------------------------*/
	media_data.track[AUDIO_INDEX].n_frames = 0;
	media_data.track[AUDIO_INDEX].first_frame = 0;
	media_data.track[AUDIO_INDEX].buffer = (char*)calloc(AUDIO_BUF_SIZE,sizeof(char));  //15s的视频数据缓存
	if(media_data.track[AUDIO_INDEX].buffer == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;
	}
	media_data.track[AUDIO_INDEX].buffer_w_pos = media_data.track[AUDIO_INDEX].buffer;
	media_data.track[AUDIO_INDEX].buffer_r_pos = media_data.track[AUDIO_INDEX].buffer;
	media_data.track[AUDIO_INDEX].buffer_size = VIDEO_BUF_SIZE;
	media_data.track[AUDIO_INDEX].size = (int *)calloc(MAX_AUDIO_FRAME*sizeof(int),sizeof(char));
	if(media_data.track[AUDIO_INDEX].size == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;	
	}

	media_data.track[AUDIO_INDEX].offset = (int *)calloc(MAX_AUDIO_FRAME*sizeof(int),sizeof(char));
	if(media_data.track[AUDIO_INDEX].offset == NULL){
		TS_ERROR_LOG("calloc failed!\n");
		return -1;	
	}

	media_data.track[AUDIO_INDEX].frames_written = 0;
	media_data.track[AUDIO_INDEX].data_start_offset = 0;
	media_data.track[AUDIO_INDEX].cc = 0;


	return 0;
}


/*******************************************************************************
*@ Description    :音频编码对外接口函数
*@ Input          :
*@ Output         :
*@ Return         :成功：0 ；失败：-1
*@ attention      :
*******************************************************************************/
int TsAEncode(void*frame,int frame_len)
{
	ts_track_data_t* 	track_data = &media_data.track[AUDIO_INDEX];
	char*				w_buf_max_pos = track_data->buffer + track_data->buffer_size;
	ts_track_media_t*	track_status = &media_stats.track[AUDIO_INDEX];
	char* 				out_buf = NULL;
	int 				out_buf_len = 0;
	
	if(TS_Audio_Encode(frame,frame_len,&out_buf,&out_buf_len) < 0)
	{
		TS_ERROR_LOG("TS_Audio_Encode failed !\n");
		return -1;
	}

	/*---# media_data 放入新的一帧数据------------------------------------------------------------*/
	if(track_data->buffer_w_pos + out_buf_len > w_buf_max_pos)
	{
		TS_ERROR_LOG("media_data.track_data[AUDIO_INDEX].buffer overflow!!\n");
		return -1;
	}
	memcpy(track_data->buffer_w_pos,out_buf,out_buf_len); 
	track_data->n_frames ++;
	track_data->buffer_w_pos +=  out_buf_len;
	track_data->size[track_data->n_frames-1] = out_buf_len;
	track_data->offset[track_data->n_frames-1] = (int)(track_data->buffer_w_pos - track_data->buffer - out_buf_len);
	/*---#-END------------------------------------------------------------------------------------*/
	
	
	/*---# media_stats 放入新的一帧数据信息-------------------------------------------------------*/
	track_status->n_frames ++;
	if(1 == track_status->n_frames)
		track_status->pts[track_status->n_frames-1] = (float)(1024.0/ts_audio_init_info.sample_rate);//给个随意初始值（不为零,1024为AAC帧样本数）
	else
		track_status->pts[track_status->n_frames-1] = track_status->pts[track_status->n_frames-2] + \
													1024.0/ts_audio_init_info.sample_rate;//加上1帧的播放时长(1024/sample_rate)
	track_status->dts[track_status->n_frames-1] = track_status->pts[track_status->n_frames-1];
	track_status->flags[track_status->n_frames-1] = 1;//audio 帧每帧都是（当做）关键帧

	/*---#--END------------------------------------------------------------------------------------*/
	
	return 0;
}

/*******************************************************************************
*@ Description    :视频编码对外接口函数
*@ Input          :
*@ Output         :
*@ Return         :成功：0 ；失败：-1
*@ attention      :
*******************************************************************************/
int TsVEncode(void*frame,int frame_len)
{

	ts_track_data_t* 	track_data = &media_data.track[VIDEO_INDEX];
	char*				w_buf_max_pos = track_data->buffer + track_data->buffer_size;
	ts_track_media_t*	track_status = &media_stats.track[VIDEO_INDEX];
	
	char* 				out_buf = NULL;
	int 				out_buf_len = 0;
	int 				is_key_frame = 0;
	
	if(TS_Video_Encode(frame,frame_len,&out_buf,&out_buf_len,&is_key_frame) < 0)
	{
		TS_ERROR_LOG("TS_Video_Encode failed !\n");
		return -1;
	}

	/*---# media_data 放入新的一帧数据------------------------------------------------------------*/
	if(track_data->buffer_w_pos + out_buf_len > w_buf_max_pos)
	{
		TS_ERROR_LOG("media_data.track_data[VIDEO_INDEX].buffer overflow!!\n");
		return -1;
	}
	memcpy(track_data->buffer_w_pos,out_buf,out_buf_len); 
	track_data->n_frames ++;
	track_data->buffer_w_pos +=  out_buf_len;
	track_data->size[track_data->n_frames-1] = out_buf_len;
	track_data->offset[track_data->n_frames-1] = (int )(track_data->buffer_w_pos - track_data->buffer - out_buf_len);
	/*---#-END------------------------------------------------------------------------------------*/
	
	
	/*---# media_stats 放入新的一帧数据信息-------------------------------------------------------*/
	track_status->n_frames ++;
	if(1 == track_status->n_frames)
		track_status->pts[track_status->n_frames-1] = (float)(1.0/ts_video_init_info.frame_rate);//给个随意初始值（一帧的时长）
	else
		track_status->pts[track_status->n_frames-1] = track_status->pts[track_status->n_frames-2] + \
													1.0/ts_video_init_info.frame_rate;//加上1帧的播放时长(90000/frame_rate)/90000
	track_status->dts[track_status->n_frames-1] = track_status->pts[track_status->n_frames-1];
	
	printf("----video pts[%d] = %f  dts[%d] = %f\n",track_status->n_frames-1,track_status->pts[track_status->n_frames-1] ,
													track_status->n_frames-1,track_status->dts[track_status->n_frames-1]);
	if(is_key_frame)
		track_status->flags[track_status->n_frames-1] = 1;
	else
		track_status->flags[track_status->n_frames-1] = 0;
	/*---#--END------------------------------------------------------------------------------------*/
	
	
	return 0;
}


/*******************************************************************************
*@ Description    : TS 混合线程主功能函数  
*@ Input          :
*@ Output         :
*@ Return         :成功：0 ；失败：-1
*@ attention      :out_buf需要上层free才能释放
*******************************************************************************/
//void* TS_remux_video_audio(void *args)
int  TS_remux_video_audio(void **out_buf,int* out_len)
{
	//pthread_detach(pthread_self());
	//等待条件成熟,记录下这次要打包的音视频数据帧 buf 的区间，支队该区间进行操作

	
	/*---#放 TS header + PAT + PMT-------------------------------------------------*/
	TS_DEBUG_LOG("put pat/pmt + video frame\n");	
	int pat_cc = 0;//pat的数量统计
	int pmt_cc = 0;//pmt的数量统计
	int ret= 0;
	
	char buf[188] = {0};
	TS_put_pat(buf, &pat_cc);
	write_buf(&ts_recoder_buf, buf,188);

	memset(buf,0,sizeof(buf));	
	TS_put_pmt(buf,&media_stats, &pmt_cc, VIDEO_stream_PID);
	write_buf(&ts_recoder_buf, buf, 188);
	
	//15S进行一次合成，执行以下流程
	/*---#先对第一帧video帧数据进行TS打包（188字节）------------------------------------------------------------*/
	ret = TS_put_data_frame(ts_recoder_buf.w_pos, &media_stats, &media_data, LEAD_TRACK, LEAD_TRACK, 1);
	ret = w_pos_add(&ts_recoder_buf,ret);
	if(ret < 0)
	{
		TS_ERROR_LOG("ts_recoder_buf is overflow!! \n");
		goto ERR;
	}
	
	/*---#将剩余的帧数据进行TS打包------------------------------------------------------------*/
	while(1)
	{
		DEBUG_LOG("put other frames\n");	
		int ct = select_current_track(&media_stats,  &media_data);
		if (ct < 0)
			break;

		ret = TS_put_data_frame(ts_recoder_buf.w_pos, &media_stats, &media_data, ct, LEAD_TRACK, ct != VIDEO_INDEX ? 6 : 1);//一次 video 放1帧，audio 放6帧
		ret = w_pos_add(&ts_recoder_buf,ret);
		if(ret < 0)
		{
			TS_ERROR_LOG("ts_recoder_buf is overflow!! \n");
			goto ERR;
		}
		
		//usleep(5);
	}

	*out_buf = ts_recoder_buf.buf;
	*out_len = (int)(ts_recoder_buf.w_pos - ts_recoder_buf.buf);
	return 0;
	//本次打包工作完成，标记该部分 buf 可用
	//循环等待下一次的打包条件成熟，同时判断要不要退出打包线程

ERR:
	//通知“帧接收线程”退出，程序异常结束
	free_buf(&ts_recoder_buf);//异常退出时ts_recoder_buf需要自己释放
	*out_buf = NULL;
	*out_len = 0;
	return -1;
	//pthread_exit(NULL);
}


void TS_recoder_exit(void)
{
			
	TS_video_exit();
	TS_audio_exit();
	
	if(media_stats.n_tracks > 0)
	{
		int i =0;
		for(i=0 ; i<media_stats.n_tracks ; i++)
		{
			printf("------media_stats.n_tracks(%d) free media_stats.track[%d]------\n",media_stats.n_tracks,i);
			if(media_stats.track[i].pts)	{free(media_stats.track[i].pts);}
			if(media_stats.track[i].dts)	{free(media_stats.track[i].dts);}
			if(media_stats.track[i].flags)	{free(media_stats.track[i].flags);}
		}
		memset(&media_stats,0,sizeof(media_stats));
	}

	if(media_data.n_tracks > 0)
	{
		int i = 0;
		for(i = 0 ; i < media_data.n_tracks ; i++)
		{
			printf("------media_data.n_tracks(%d) free media_data.track[%d]------\n",media_data.n_tracks,i);
			if(media_data.track[i].buffer)	{free(media_data.track[i].buffer);}
			if(media_data.track[i].size)	{free(media_data.track[i].size);}
			if(media_data.track[i].offset)	{free(media_data.track[i].offset);}
		}
		memset(&media_data,0,sizeof(media_data));
	}

	/* 该部分buf需要返回给上层，由上层释放
	if(ts_recoder_buf.buf) 
	{
		free(ts_recoder_buf.buf);
		ts_recoder_buf.buf = NULL;
		memset(&ts_recoder_buf,0,sizeof(ts_recoder_buf));
	}
	*/

}

void ts_global_variable_reset(void)
{
	ts_audio_global_variable_reset();
	TS_video_global_variable_reset();
	
	memset(&ts_recoder_buf,0,sizeof(ts_recoder_buf));
	memset(&media_stats,0,sizeof(media_stats));
	memset(&media_data,0,sizeof(media_stats));

}
	

