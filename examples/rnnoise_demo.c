
#include <stdio.h>
#include "rnnoise.h"
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#define DR_MP3_IMPLEMENTATION

#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION

#include "dr_wav.h"

#if   defined(__APPLE__)
# include <mach/mach_time.h>
#elif defined(_WIN32)
# define WIN32_LEAN_AND_MEAN

# include <windows.h>

#else // __linux

# include <time.h>

# ifndef  CLOCK_MONOTONIC //_RAW
#  define CLOCK_MONOTONIC CLOCK_REALTIME
# endif
#endif

static
uint64_t nanotimer() {
    static int ever = 0;
#if defined(__APPLE__)
    static mach_timebase_info_data_t frequency;
    if (!ever) {
        if (mach_timebase_info(&frequency) != KERN_SUCCESS) {
            return 0;
        }
        ever = 1;
    }
    return  (mach_absolute_time() * frequency.numer / frequency.denom);
#elif defined(_WIN32)
    static LARGE_INTEGER frequency;
    if (!ever) {
        QueryPerformanceFrequency(&frequency);
        ever = 1;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (t.QuadPart * (uint64_t) 1e9) / frequency.QuadPart;
#else // __linux
    struct timespec t = {0};
    if (!ever) {
        if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
            return 0;
        }
        ever = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec * (uint64_t) 1e9) + t.tv_nsec;
#endif
}

static double now() {
    static uint64_t epoch = 0;
    if (!epoch) {
        epoch = nanotimer();
    }
    return (nanotimer() - epoch) / 1e9;
};

static double calcElapsed(double start, double end) {
    double took = -start;
    return took + end;
}


void wavWrite_f32(char *filename, float *buffer, int sampleRate, uint32_t totalSampleCount, uint32_t channels) {
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = channels;
    format.sampleRate = (drwav_uint32) sampleRate;
    format.bitsPerSample = 32;
    for (int32_t i = 0; i < totalSampleCount; ++i) {
        buffer[i] = drwav_clamp(buffer[i], -32768, 32767) * (1.0f / 32768.0f);
    }
    drwav *pWav = drwav_open_file_write(filename, &format);
    if (pWav) {
        drwav_uint64 samplesWritten = drwav_write(pWav, totalSampleCount, buffer);
        drwav_uninit(pWav);
        if (samplesWritten != totalSampleCount) {
            fprintf(stderr, "write file [%s] error.\n", filename);
            exit(1);
        }
    }
}

float *wavRead_f32(const char *filename, uint32_t *sampleRate, uint64_t *sampleCount, uint32_t *channels) {
    drwav_uint64 totalSampleCount = 0;
    float *input = drwav_open_file_and_read_pcm_frames_f32(filename, channels, sampleRate, &totalSampleCount);
    if (input == NULL) {
        drmp3_config pConfig;
        input = drmp3_open_file_and_read_f32(filename, &pConfig, &totalSampleCount);
        if (input != NULL) {
            *channels = pConfig.outputChannels;
            *sampleRate = pConfig.outputSampleRate;
        }
    }
    if (input == NULL) {
        fprintf(stderr, "read file [%s] error.\n", filename);
        exit(1);
    }
    *sampleCount = totalSampleCount * (*channels);
    for (int32_t i = 0; i < *sampleCount; ++i) {
        input[i] = input[i] * 32768.0f;
    }
    return input;
}


void splitpath(const char *path, char *drv, char *dir, char *name, char *ext) {
    const char *end;
    const char *p;
    const char *s;
    if (path[0] && path[1] == ':') {
        if (drv) {
            *drv++ = *path++;
            *drv++ = *path++;
            *drv = '\0';
        }
    } else if (drv)
        *drv = '\0';
    for (end = path; *end && *end != ':';)
        end++;
    for (p = end; p > path && *--p != '\\' && *p != '/';)
        if (*p == '.') {
            end = p;
            break;
        }
    if (ext)
        for (s = end; (*ext = *s++);)
            ext++;
    for (p = end; p > path;)
        if (*--p == '\\' || *p == '/') {
            p++;
            break;
        }
    if (name) {
        for (s = p; s < end;)
            *name++ = *s++;
        *name = '\0';
    }
    if (dir) {
        for (s = path; s < p;)
            *dir++ = *s++;
        *dir = '\0';
    }
}


uint64_t Resample_f32(const float *input, float *output, int inSampleRate, int outSampleRate, uint64_t inputSize,
                      uint32_t channels
) {
    if (input == NULL)
        return 0;
    uint64_t outputSize = inputSize * outSampleRate / inSampleRate;
    if (output == NULL)
        return outputSize;
    double stepDist = ((double) inSampleRate / (double) outSampleRate);
    const uint64_t fixedFraction = (1LL << 32);
    const double normFixed = (1.0 / (1LL << 32));
    uint64_t step = ((uint64_t) (stepDist * fixedFraction + 0.5));
    uint64_t curOffset = 0;
    for (uint32_t i = 0; i < outputSize; i += 1) {
        for (uint32_t c = 0; c < channels; c += 1) {
            *output++ = (float) (input[c] + (input[c + channels] - input[c]) * (
                    (double) (curOffset >> 32) + ((curOffset & (fixedFraction - 1)) * normFixed)
            )
            );
        }
        curOffset += step;
        input += (curOffset >> 32) * channels;
        curOffset &= (fixedFraction - 1);
    }
    return outputSize;
}

void denoise_proc(float *input, uint64_t sampleCount, uint32_t sampleRate, uint32_t channels) {
    uint32_t targetFrameSize = 480;
    uint32_t targetSampleRate = 48000;
    uint32_t perFrameSize = sampleRate / 100;
    float *frameBuffer = (float *) malloc(sizeof(*frameBuffer) * (channels + 1) * targetFrameSize);
    float *processBuffer = frameBuffer + targetFrameSize * channels;
    DenoiseState **sts = malloc(channels * sizeof(DenoiseState *));
    if (sts == NULL || frameBuffer == NULL) {
        if (sts)
            free(sts);
        if (frameBuffer)
            free(frameBuffer);
        fprintf(stderr, "malloc error.\n");
        return;
    }
    for (int i = 0; i < channels; i++) {
        sts[i] = rnnoise_create();

        if (sts[i] == NULL) {
            for (int x = 0; x < i; x++) {
                if (sts[x]) {
                    rnnoise_destroy(sts[x]);
                }
            }
            free(sts);
            free(frameBuffer);
            return;
        }
    }
    size_t frameStep = channels * perFrameSize;
    uint64_t frames = sampleCount / frameStep;
    uint64_t lastFrameSize = (sampleCount % frameStep) / channels;

    for (int i = 0; i < frames; ++i) {
        Resample_f32(input, frameBuffer, sampleRate, targetSampleRate,
                     perFrameSize, channels);
        for (int c = 0; c < channels; c++) {
            for (int k = 0; k < targetFrameSize; k++)
                processBuffer[k] = frameBuffer[k * channels + c];
            rnnoise_process_frame(sts[c], processBuffer, processBuffer);
            for (int k = 0; k < targetFrameSize; k++)
                frameBuffer[k * channels + c] = processBuffer[k];
        }
        Resample_f32(frameBuffer, input, targetSampleRate, sampleRate, targetFrameSize, channels);
        input += frameStep;
    }
    if (lastFrameSize != 0) {
        memset(frameBuffer, 0, targetFrameSize * channels * sizeof(float));
        uint64_t lastReasmpleSize = Resample_f32(input, frameBuffer, sampleRate,
                                                 targetSampleRate,
                                                 lastFrameSize, channels);
        for (int c = 0; c < channels; c++) {
            for (int k = 0; k < targetFrameSize; k++)
                processBuffer[k] = frameBuffer[k * channels + c];
/*
** This is where to set the max_atten on the fly
** Set max_atten to 0dB to turn off rnnoise (useful for real-time demo):
**     rnnoise_set_current_max_atten(sts[i], 0.0f); 
** Reset max_atten to initial value
**     rnnoise_set_current_max_atten(sts[i], rnnoise_get_init_max_attenuation()); 
*/				
            rnnoise_process_frame(sts[c], processBuffer, processBuffer);
            for (int k = 0; k < targetFrameSize; k++)
                frameBuffer[k * channels + c] = processBuffer[k];
        }
        Resample_f32(frameBuffer, input, targetSampleRate, sampleRate, lastReasmpleSize,
                     channels);
    }
    for (int i = 0; i < channels; i++) {
        if (sts[i]) {
            rnnoise_destroy(sts[i]);
        }
    }
    free(sts);
    free(frameBuffer);
}

void rnnDeNoise(char *in_file, char *out_file) {
    uint32_t sampleRate = 0;
    uint64_t sampleCount = 0;
    uint32_t channels = 0;
    float *buffer = wavRead_f32(in_file, &sampleRate, &sampleCount, &channels);
    if (buffer != NULL) {
        double startTime = now();
        denoise_proc(buffer, sampleCount, sampleRate, channels);
        double time_interval = calcElapsed(startTime, now());
        printf("Total processing time: %5.2f ms\n ", (time_interval * 1000));
        wavWrite_f32(out_file, buffer, sampleRate, (uint32_t) sampleCount, channels);
        free(buffer);
    }
}


int main(int argc, char **argv) {
	float max_atten = 0.1f;
	char *out_file;
	char *in_file = argv[1];

//    printf("\n***  Github rnnoise from J-M Valin et al\n");
//    printf("blog:http://cpuimage.cnblogs.com/\n");
//    printf("e-mail:gaozhihan@vip.qq.com\n");

    if (argc < 2) {
        printf("  usage:\n");
        printf("  ./rnnoise_demo input.wav\n");
        printf("  ./rnnoise_demo input.mp3\n");
        printf("    or\n");
        printf("  ./rnnoise_demo input.wav output.wav\n");
        printf("  ./rnnoise_demo input.mp3 output.wav\n");
        printf("    or\n");
		printf("  ./rnnoise_demo input.wav output.wav 10 (for 10dB attenuation; default=%2.0f)\n", 
			-10.0f*log10(rnnoise_get_init_max_atten()));
		printf("  ./rnnoise_demo input.mp3 output.mp3 10 (for 10dB attenuation; default=%2.0f)\n", 
			-10.0f*log10(rnnoise_get_init_max_atten()));
        return -1;
    }

    if (argc > 2) {
		out_file = argv[2];
		if (argc > 3) {
			rnnoise_set_init_max_atten(atof(argv[3]));
		}
    } else {
        char drive[3];
        char dir[256];
        char fname[256];
        char ext[256];
        char out_file[1024];
        splitpath(in_file, drive, dir, fname, ext);
        sprintf(out_file, "%s%s%s_out.wav", drive, dir, fname);
    }

	rnnDeNoise(in_file, out_file);
    return 0;
}
