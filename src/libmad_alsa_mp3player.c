#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <mad.h>
#include <id3tag.h>
#include <alsa/asoundlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define INPUT_BUFFER_SIZE	(5 * 8192)
#define OUTPUT_BUFFER_SIZE	(1152 * 8)	
//#define OUTPUT_BUFFER_SIZE	(1152 * 4)	

snd_pcm_t			*handle;
snd_pcm_uframes_t	frames;
pthread_mutex_t		lock;
pthread_cond_t		empty;
pthread_cond_t		full;
int					finished;
int					rate;
unsigned char		OutputBuffer[OUTPUT_BUFFER_SIZE];

static unsigned short MadFixedToUshort(mad_fixed_t Fixed)
{
	Fixed = Fixed >> (MAD_F_FRACBITS - 15);
	return((unsigned short)Fixed);
}

int snd_init(void)
{
	int err;
	snd_pcm_hw_params_t *params;
	printf("mad mp3 player %s\n", __TIME__);
	
	/* open the pcm device */
	if ((err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		printf("failed to open pcm device \"default\" (%s)\n", snd_strerror(err));
		return -1;
	}

	/* alloc memory space for hardware parameter structure*/
	if ((err = snd_pcm_hw_params_malloc(&params)) < 0) {
		printf("cannot allocate hardware parameter structure (%s)\n", snd_strerror(err));
		return -1;
	}

	/* 使用默认数据初始化声卡参数结构体 */
	if ((err = snd_pcm_hw_params_any(handle, params)) < 0) {
		printf("failed to initialize hardware parameter structure (%s)\n", snd_strerror(err));
		return -1;
	}

	/* 设置声卡的访问参数为交错访问 */
	if ((err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		printf("cannot set access type (%s)\n", snd_strerror(err));
		return -1;
	}

	/* 设置声卡的数据格式为有符号的32位小端数 */
	if ((err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S32_LE)) < 0) {
	//if ((err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE)) < 0) {
	//if ((err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_U16_LE)) < 0) {
		printf("cannot set sample format (%s)\n", snd_strerror(err));
		return -1;
	}

	/* 设置声卡的采样率为44100 */
	rate = 44100;
	if ((err = snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0)) < 0) {
		printf("cannot set sample format (%s)\n", snd_strerror(err));
		return -1;
	}
	printf("rate: %d\n", rate);

	/* 设置声卡的声道为2 */
	int channels = 2;
	if ((err = snd_pcm_hw_params_set_channels(handle, params, channels)) < 0) {
		printf("cannot set channel count (%s)\n", snd_strerror(err));
		return -1;
	}

	frames = 1152;
	//frames = 512;
	if ((err = snd_pcm_hw_params_set_period_size_near(handle, params, &frames, 0)) < 0) {
		printf("cannot set period size (%s)\n", snd_strerror(err));
		return -1;
	}
	printf("frames: %d\n", frames);
		
	if ((err = snd_pcm_hw_params(handle, params)) < 0) {
		printf("cannot set parameters (%s)\n", snd_strerror(err));
		return -1;
	}

	// snd_pcm_hw_params_free(params);
	if ((err = snd_pcm_prepare(handle)) < 0) {
		printf("cannont prepare audio interface for use (%s)\n", snd_strerror(err));
		return -1;
	}

	return 0;
}

/* 解码函数 */
void *decode(void *pthread_arg)
{
	struct mad_stream 		Stream;
	struct mad_frame 		Frame;
	struct mad_synth 		Synth;
	//mad_timer_t				Timer;
	unsigned char 			Mp3_InputBuffer[INPUT_BUFFER_SIZE];
	unsigned char			*OutputPtr = OutputBuffer;
	unsigned char *const	OutputBufferEnd = OutputBuffer + OUTPUT_BUFFER_SIZE;
	int						i, err;
	int						fd = (int)pthread_arg;

	/* libmad初始化 */
	mad_stream_init(&Stream);
	mad_frame_init(&Frame);
	mad_synth_init(&Synth);

	/* 开始解码 */
	do {
		/* 如果缓冲区空了或不足一帧数据, 就向缓冲区填充数据 */
		if(Stream.buffer == NULL || Stream.error == MAD_ERROR_BUFLEN) {
			size_t 			BufferSize;		/* 缓冲区大小 */
			size_t			Remaining;		/* 帧剩余数据 */
			unsigned char	*BufferStart;	/* 头指针 */

			if (Stream.next_frame != NULL) {
				printf("还有存货\n");

				/* 把剩余没解码完的数据补充到这次的缓冲区中 */
				Remaining = Stream.bufend - Stream.next_frame;
				memmove(Mp3_InputBuffer, Stream.next_frame, Remaining);
				BufferStart = Mp3_InputBuffer + Remaining;
				BufferSize = INPUT_BUFFER_SIZE - Remaining;
			} else {
				printf("消耗完毕\n");

				/* 设置了缓冲区地址, 但还没有填充数据 */
				BufferSize = INPUT_BUFFER_SIZE;
				BufferStart = Mp3_InputBuffer;
				Remaining = 0;
			}

			/* 从文件中读取数据并填充缓冲区 */
			BufferSize = read(fd, BufferStart, BufferSize);
			if (BufferSize <= 0) {
				printf("文件读取失败\n");
				exit(-1);
			}

			mad_stream_buffer(&Stream, Mp3_InputBuffer, BufferSize + Remaining);
			Stream.error = 0;
		}

		if (err = mad_frame_decode(&Frame, &Stream)) {
			printf("解码出错: %x\n", Stream.error);

			if (MAD_RECOVERABLE(Stream.error)) {
				printf("可恢复\n");
				continue;
			} else {
				if (Stream.error == MAD_ERROR_BUFLEN) {
					printf("buffer数据不足一帧, 需要继续填充\n");

					continue; /* buffer解码光了, 需要继续填充了 */
				} else if (Stream.error == MAD_ERROR_LOSTSYNC) {
					printf("丢失同步\n");

					int tagsize;
					tagsize = id3_tag_query(Stream.this_frame, Stream.bufend - Stream.this_frame);
					if (tagsize > 0) {
						mad_stream_skip(&Stream, tagsize);
					}
					continue;
				} else {
					printf("严重错误，停止解码\n");
					exit(-1);
				}
			}
		}
		/* 设置每帧的播放时间 */
		//mad_timer_add(&Timer, Frame.header.duration);
		/* 解码成音频数据 */
		mad_synth_frame(&Synth, &Frame);


		pthread_mutex_lock(&lock);
		if (finished) {
			pthread_cond_wait(&empty, &lock);
		}
		/*printf("Synth.pcm.length: %d\n", Synth.pcm.length);
		printf("samplerate: %d\n", Synth.pcm.samplerate);
		 if (rate != Synth.pcm.samplerate) {
			if ((err = snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0)) < 0) {
				printf("cannot set sample format (%s)\n", snd_strerror(err));
				return -1;
			}
			rate = Synth.pcm.samplerate;
			if ((err = snd_pcm_hw_params(handle, params)) < 0) {
				printf("cannot set parameters (%s)\n", snd_strerror(err));
				return -1;
			}
			if ((err = snd_pcm_prepare(handle)) < 0) {
				printf("cannont prepare audio interface for use (%s)\n", snd_strerror(err));
				return -1;
			}
		}*/
		
		/* 解码后的音频数据转换成16位的数据 */
		for (i = 0; i < Synth.pcm.length; i++) {
			//unsigned short Sample;
			signed int Sample;
			Sample = Synth.pcm.samples[0][i];
			//Sample = MadFixedToUshort(Synth.pcm.samples[0][i]);
			*(OutputPtr++) = (Sample & 0xff);
			*(OutputPtr++) = (Sample >> 8);
			*(OutputPtr++) = (Sample >> 16);
			*(OutputPtr++) = (Sample >> 24);

			if (MAD_NCHANNELS(&Frame.header) == 2) {
				Sample = Synth.pcm.samples[1][i];
				//Sample = MadFixedToUshort(Synth.pcm.samples[1][i]);
				*(OutputPtr++) = (Sample & 0xff);
				*(OutputPtr++) = (Sample >> 8);
				*(OutputPtr++) = (Sample >> 16);
				*(OutputPtr++) = (Sample >> 24);
			}

			/* 输出缓冲区填充满 */
			if (OutputPtr >= OutputBufferEnd) {
				OutputPtr = OutputBuffer;
				finished = 1;
				pthread_mutex_unlock(&lock);
				pthread_cond_signal(&full);
			}
		}
	} while(1);

	mad_synth_finish(&Synth);
	mad_frame_finish(&Frame);
	mad_stream_finish(&Stream);
	
	return;
}

long writebuf(snd_pcm_t *handle, char *buf, long len, size_t *frames)
{
	long r;
	while (len > 0) {
		r = snd_pcm_writei(handle, buf, 4);
		if (r == -EAGAIN)
			continue;
		if (r < 4) {
			printf("write = %li\n", r);
		}
		//printf("write = %li\n", r);
		if (r < 0)
			return r;
		// showstat(handle, 0);
		//buf += r * 4;
		buf += r * 8;
		len -= r;
		*frames += r;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int			mp3_fd;
	int			err = -1;
	pthread_t	decode_thread;

	if (!argv[1]) {
		printf("plz input a mp3 file\n");
		return -1;
	}

	mp3_fd = open(argv[1], O_RDONLY);
	if (!mp3_fd) {
		perror("failed to open file ");
		return -1;
	}

	if (snd_init() < 0) {
		printf("faile to init snd card\n");
		return -1;
	}

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&empty);
	pthread_cond_init(&full);
	pthread_create(&decode_thread, NULL, decode, (void*)mp3_fd);
	
	while (1) {
		pthread_mutex_lock(&lock);
		if (!finished) {
			pthread_cond_wait(&full, &lock);
		}
		
		//if ((err = snd_pcm_writei(handle, OutputBuffer, frames)) < 0) {
		size_t count;
		if ((err = writebuf(handle, OutputBuffer, 1152, &count)) < 0) {
			printf("write error: %s, errno: %d\n", snd_strerror(err), err);
			if (err == -EPIPE) {
				int errb;
				errb = snd_pcm_recover(handle, err, 0);
				if (errb < 0) {
					printf("failed to recover from underrun\n");
					return -1;
				} else {
					printf("recover\n");
				}
				snd_pcm_prepare(handle);
			}
		}

		finished = 0;
		pthread_mutex_unlock(&lock);
		pthread_cond_signal(&empty);
	}
	
	return 0;
}
