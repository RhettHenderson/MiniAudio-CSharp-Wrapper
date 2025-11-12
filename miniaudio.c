#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#ifndef MA_WRAPPER_API
#if defined(_WIN32)
#define MA_WRAPPER_API __declspec(dllexport)
#else
#define MA_WRAPPER_API __attribute__((visibility("default")))
#endif
#endif

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#include "miniaudio.h"

typedef struct
{
    ma_context context;
    ma_device device;
    ma_pcm_rb ringBuffer;
    ma_format format;
    ma_uint32 channels;
    ma_uint32 sampleRate;
    ma_uint32 bufferSizeInFrames;
    ma_uint32 bytesPerFrame;
    ma_bool32 isStarted;
} ma_microphone;

typedef struct
{
    ma_context context;
    ma_device device;
    ma_pcm_rb ringBuffer;
    ma_format format;
    ma_uint32 channels;
    ma_uint32 sampleRate;
    ma_uint32 bufferSizeInFrames;
    ma_uint32 bytesPerFrame;
    ma_bool32 isStarted;
} ma_speaker;

static ma_result ma_init_context_for_platform(ma_context* pContext)
{
#if defined(_WIN32)
    const ma_backend backends[] = {
        ma_backend_wasapi,
        ma_backend_dsound,
        ma_backend_winmm
    };
    return ma_context_init(backends, (ma_uint32)ma_countof(backends), NULL, pContext);
#elif defined(__linux__)
    const ma_backend backends[] = {
        ma_backend_pulseaudio,
        ma_backend_alsa,
        ma_backend_jack
    };
    return ma_context_init(backends, (ma_uint32)ma_countof(backends), NULL, pContext);
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
    const ma_backend backends[] = {
        ma_backend_coreaudio
    };
    return ma_context_init(backends, (ma_uint32)ma_countof(backends), NULL, pContext);
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    const ma_backend backends[] = {
        ma_backend_coreaudio
    };
    return ma_context_init(backends, (ma_uint32)ma_countof(backends), NULL, pContext);
#elif defined(__ANDROID__)
    const ma_backend backends[] = {
        ma_backend_aaudio,
        ma_backend_opensl,
        ma_backend_null
    };
    return ma_context_init(backends, (ma_uint32)ma_countof(backends), NULL, pContext);
#else
    return ma_context_init(NULL, 0, NULL, pContext);
#endif
}

static ma_uint32 ma_calculate_default_buffer_size(ma_uint32 sampleRate, ma_uint32 periodSizeInFrames)
{
    if (periodSizeInFrames != 0) {
        return periodSizeInFrames * 4;
    }

    if (sampleRate == 0) {
        sampleRate = 48000;
    }

    return ma_clamp(sampleRate / 20, 1024U, sampleRate);
}

static void ma_microphone_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    (void)pOutput;

    ma_microphone* pMicrophone = (ma_microphone*)pDevice->pUserData;
    if (pMicrophone == NULL || pInput == NULL || frameCount == 0) {
        return;
    }

    const ma_uint32 bytesPerFrame = pMicrophone->bytesPerFrame;
    const ma_uint8* pInputBytes = (const ma_uint8*)pInput;
    ma_uint32 framesProcessed = 0;

    while (framesProcessed < frameCount) {
        ma_uint32 framesToWrite = frameCount - framesProcessed;
        void* pWritePtr = NULL;

        if (ma_pcm_rb_acquire_write(&pMicrophone->ringBuffer, &framesToWrite, &pWritePtr) != MA_SUCCESS || framesToWrite == 0) {
            /* Buffer is full, drop the remainder to avoid blocking the callback. */
            break;
        }

        ma_copy_memory_64(pWritePtr, pInputBytes + (framesProcessed * bytesPerFrame), (ma_uint64)framesToWrite * bytesPerFrame);
        ma_pcm_rb_commit_write(&pMicrophone->ringBuffer, framesToWrite);
        framesProcessed += framesToWrite;
    }
}

static void ma_speaker_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    (void)pInput;

    ma_speaker* pSpeaker = (ma_speaker*)pDevice->pUserData;
    if (pSpeaker == NULL || pOutput == NULL || frameCount == 0) {
        return;
    }

    ma_uint8* pOutputBytes = (ma_uint8*)pOutput;
    const ma_uint32 bytesPerFrame = pSpeaker->bytesPerFrame;
    ma_uint32 framesProcessed = 0;

    while (framesProcessed < frameCount) {
        ma_uint32 framesToRead = frameCount - framesProcessed;
        void* pReadPtr = NULL;

        if (ma_pcm_rb_acquire_read(&pSpeaker->ringBuffer, &framesToRead, &pReadPtr) != MA_SUCCESS || framesToRead == 0) {
            ma_silence_pcm_frames(pOutputBytes + (framesProcessed * bytesPerFrame), frameCount - framesProcessed, pSpeaker->format, pSpeaker->channels);
            break;
        }

        ma_copy_memory_64(pOutputBytes + (framesProcessed * bytesPerFrame), pReadPtr, (ma_uint64)framesToRead * bytesPerFrame);
        ma_pcm_rb_commit_read(&pSpeaker->ringBuffer, framesToRead);
        framesProcessed += framesToRead;
    }
}

static ma_result ma_microphone_init(ma_microphone* pMicrophone, ma_uint32 sampleRate, ma_uint32 channels, ma_format format, ma_uint32 bufferSizeInFrames)
{
    if (pMicrophone == NULL) {
        return MA_INVALID_ARGS;
    }

    ma_zero_memory_64(pMicrophone, (ma_uint64)sizeof(*pMicrophone));

    ma_result result = ma_init_context_for_platform(&pMicrophone->context);
    if (result != MA_SUCCESS) {
        return result;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.dataCallback = ma_microphone_data_callback;
    config.pUserData = pMicrophone;
    config.capture.format = (format == ma_format_unknown) ? ma_format_f32 : format;
    config.capture.channels = (channels == 0) ? 1 : channels;
    config.sampleRate = sampleRate;

    result = ma_device_init(&pMicrophone->context, &config, &pMicrophone->device);
    if (result != MA_SUCCESS) {
        ma_context_uninit(&pMicrophone->context);
        return result;
    }

    pMicrophone->format = (pMicrophone->device.capture.internalFormat != ma_format_unknown) ? pMicrophone->device.capture.internalFormat : config.capture.format;
    pMicrophone->channels = (pMicrophone->device.capture.internalChannels != 0) ? pMicrophone->device.capture.internalChannels : config.capture.channels;
    pMicrophone->sampleRate = (pMicrophone->device.sampleRate != 0) ? pMicrophone->device.sampleRate : ((sampleRate != 0) ? sampleRate : 48000);
    pMicrophone->bytesPerFrame = ma_get_bytes_per_frame(pMicrophone->format, pMicrophone->channels);

    if (bufferSizeInFrames == 0) {
        bufferSizeInFrames = ma_calculate_default_buffer_size(pMicrophone->sampleRate, pMicrophone->device.capture.internalPeriodSizeInFrames);
    }

    pMicrophone->bufferSizeInFrames = bufferSizeInFrames;

    result = ma_pcm_rb_init(pMicrophone->format, pMicrophone->channels, bufferSizeInFrames, NULL, NULL, &pMicrophone->ringBuffer);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&pMicrophone->device);
        ma_context_uninit(&pMicrophone->context);
        return result;
    }

    return MA_SUCCESS;
}

static void ma_microphone_uninit(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return;
    }

    if (pMicrophone->isStarted) {
        ma_device_stop(&pMicrophone->device);
        pMicrophone->isStarted = MA_FALSE;
    }

    ma_pcm_rb_uninit(&pMicrophone->ringBuffer);
    ma_device_uninit(&pMicrophone->device);
    ma_context_uninit(&pMicrophone->context);
}

static ma_result ma_speaker_init(ma_speaker* pSpeaker, ma_uint32 sampleRate, ma_uint32 channels, ma_format format, ma_uint32 bufferSizeInFrames)
{
    if (pSpeaker == NULL) {
        return MA_INVALID_ARGS;
    }

    ma_zero_memory_64(pSpeaker, (ma_uint64)sizeof(*pSpeaker));

    ma_result result = ma_init_context_for_platform(&pSpeaker->context);
    if (result != MA_SUCCESS) {
        return result;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.dataCallback = ma_speaker_data_callback;
    config.pUserData = pSpeaker;
    config.playback.format = (format == ma_format_unknown) ? ma_format_f32 : format;
    config.playback.channels = (channels == 0) ? 2 : channels;
    config.sampleRate = sampleRate;

    result = ma_device_init(&pSpeaker->context, &config, &pSpeaker->device);
    if (result != MA_SUCCESS) {
        ma_context_uninit(&pSpeaker->context);
        return result;
    }

    pSpeaker->format = (pSpeaker->device.playback.internalFormat != ma_format_unknown) ? pSpeaker->device.playback.internalFormat : config.playback.format;
    pSpeaker->channels = (pSpeaker->device.playback.internalChannels != 0) ? pSpeaker->device.playback.internalChannels : config.playback.channels;
    pSpeaker->sampleRate = (pSpeaker->device.sampleRate != 0) ? pSpeaker->device.sampleRate : ((sampleRate != 0) ? sampleRate : 48000);
    pSpeaker->bytesPerFrame = ma_get_bytes_per_frame(pSpeaker->format, pSpeaker->channels);

    if (bufferSizeInFrames == 0) {
        bufferSizeInFrames = ma_calculate_default_buffer_size(pSpeaker->sampleRate, pSpeaker->device.playback.internalPeriodSizeInFrames);
    }

    pSpeaker->bufferSizeInFrames = bufferSizeInFrames;

    result = ma_pcm_rb_init(pSpeaker->format, pSpeaker->channels, bufferSizeInFrames, NULL, NULL, &pSpeaker->ringBuffer);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&pSpeaker->device);
        ma_context_uninit(&pSpeaker->context);
        return result;
    }

    return MA_SUCCESS;
}

static void ma_speaker_uninit(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return;
    }

    if (pSpeaker->isStarted) {
        ma_device_stop(&pSpeaker->device);
        pSpeaker->isStarted = MA_FALSE;
    }

    ma_pcm_rb_uninit(&pSpeaker->ringBuffer);
    ma_device_uninit(&pSpeaker->device);
    ma_context_uninit(&pSpeaker->context);
}

MA_WRAPPER_API ma_microphone* ma_microphone_create(ma_uint32 sampleRate, ma_uint32 channels, ma_format format, ma_uint32 bufferSizeInFrames)
{
    ma_microphone* pMicrophone = (ma_microphone*)ma_malloc(sizeof(*pMicrophone), NULL);
    if (pMicrophone == NULL) {
        return NULL;
    }

    if (ma_microphone_init(pMicrophone, sampleRate, channels, format, bufferSizeInFrames) != MA_SUCCESS) {
        ma_free(pMicrophone, NULL);
        return NULL;
    }

    return pMicrophone;
}

MA_WRAPPER_API void ma_microphone_destroy(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return;
    }

    ma_microphone_uninit(pMicrophone);
    ma_free(pMicrophone, NULL);
}

MA_WRAPPER_API ma_result ma_microphone_start(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return MA_INVALID_ARGS;
    }

    if (pMicrophone->isStarted) {
        return MA_SUCCESS;
    }

    ma_result result = ma_device_start(&pMicrophone->device);
    if (result == MA_SUCCESS) {
        pMicrophone->isStarted = MA_TRUE;
    }

    return result;
}

MA_WRAPPER_API ma_result ma_microphone_stop(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return MA_INVALID_ARGS;
    }

    if (!pMicrophone->isStarted) {
        return MA_SUCCESS;
    }

    ma_result result = ma_device_stop(&pMicrophone->device);
    if (result == MA_SUCCESS) {
        pMicrophone->isStarted = MA_FALSE;
    }

    return result;
}

MA_WRAPPER_API ma_uint32 ma_microphone_read(ma_microphone* pMicrophone, void* pFramesOut, ma_uint32 frameCount)
{
    if (pMicrophone == NULL || pFramesOut == NULL || frameCount == 0) {
        return 0;
    }

    ma_uint8* pDst = (ma_uint8*)pFramesOut;
    ma_uint32 framesReadTotal = 0;

    while (framesReadTotal < frameCount) {
        ma_uint32 framesToRead = frameCount - framesReadTotal;
        void* pReadPtr = NULL;

        if (ma_pcm_rb_acquire_read(&pMicrophone->ringBuffer, &framesToRead, &pReadPtr) != MA_SUCCESS || framesToRead == 0) {
            break;
        }

        ma_copy_memory_64(pDst + (framesReadTotal * pMicrophone->bytesPerFrame), pReadPtr, (ma_uint64)framesToRead * pMicrophone->bytesPerFrame);
        ma_pcm_rb_commit_read(&pMicrophone->ringBuffer, framesToRead);
        framesReadTotal += framesToRead;
    }

    return framesReadTotal;
}

MA_WRAPPER_API ma_uint32 ma_microphone_available_frames(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return 0;
    }

    return ma_pcm_rb_available_read(&pMicrophone->ringBuffer);
}

MA_WRAPPER_API ma_format ma_microphone_get_format(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return ma_format_unknown;
    }

    return pMicrophone->format;
}

MA_WRAPPER_API ma_uint32 ma_microphone_get_channels(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return 0;
    }

    return pMicrophone->channels;
}

MA_WRAPPER_API ma_uint32 ma_microphone_get_sample_rate(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return 0;
    }

    return pMicrophone->sampleRate;
}

MA_WRAPPER_API ma_speaker* ma_speaker_create(ma_uint32 sampleRate, ma_uint32 channels, ma_format format, ma_uint32 bufferSizeInFrames)
{
    ma_speaker* pSpeaker = (ma_speaker*)ma_malloc(sizeof(*pSpeaker), NULL);
    if (pSpeaker == NULL) {
        return NULL;
    }

    if (ma_speaker_init(pSpeaker, sampleRate, channels, format, bufferSizeInFrames) != MA_SUCCESS) {
        ma_free(pSpeaker, NULL);
        return NULL;
    }

    return pSpeaker;
}

MA_WRAPPER_API void ma_speaker_destroy(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return;
    }

    ma_speaker_uninit(pSpeaker);
    ma_free(pSpeaker, NULL);
}

MA_WRAPPER_API ma_result ma_speaker_start(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return MA_INVALID_ARGS;
    }

    if (pSpeaker->isStarted) {
        return MA_SUCCESS;
    }

    ma_result result = ma_device_start(&pSpeaker->device);
    if (result == MA_SUCCESS) {
        pSpeaker->isStarted = MA_TRUE;
    }

    return result;
}

MA_WRAPPER_API ma_result ma_speaker_stop(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return MA_INVALID_ARGS;
    }

    if (!pSpeaker->isStarted) {
        return MA_SUCCESS;
    }

    ma_result result = ma_device_stop(&pSpeaker->device);
    if (result == MA_SUCCESS) {
        pSpeaker->isStarted = MA_FALSE;
    }

    return result;
}

MA_WRAPPER_API ma_uint32 ma_speaker_write(ma_speaker* pSpeaker, const void* pFrames, ma_uint32 frameCount)
{
    if (pSpeaker == NULL || pFrames == NULL || frameCount == 0) {
        return 0;
    }

    const ma_uint8* pSrc = (const ma_uint8*)pFrames;
    ma_uint32 framesWrittenTotal = 0;

    while (framesWrittenTotal < frameCount) {
        ma_uint32 framesToWrite = frameCount - framesWrittenTotal;
        void* pWritePtr = NULL;

        if (ma_pcm_rb_acquire_write(&pSpeaker->ringBuffer, &framesToWrite, &pWritePtr) != MA_SUCCESS || framesToWrite == 0) {
            break;
        }

        ma_copy_memory_64(pWritePtr, pSrc + (framesWrittenTotal * pSpeaker->bytesPerFrame), (ma_uint64)framesToWrite * pSpeaker->bytesPerFrame);
        ma_pcm_rb_commit_write(&pSpeaker->ringBuffer, framesToWrite);
        framesWrittenTotal += framesToWrite;
    }

    return framesWrittenTotal;
}

MA_WRAPPER_API ma_uint32 ma_speaker_available_frames(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return 0;
    }

    return ma_pcm_rb_available_write(&pSpeaker->ringBuffer);
}

MA_WRAPPER_API ma_format ma_speaker_get_format(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return ma_format_unknown;
    }

    return pSpeaker->format;
}

MA_WRAPPER_API ma_uint32 ma_speaker_get_channels(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return 0;
    }

    return pSpeaker->channels;
}

MA_WRAPPER_API ma_uint32 ma_speaker_get_sample_rate(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return 0;
    }

    return pSpeaker->sampleRate;
}

MA_WRAPPER_API void ma_speaker_flush(ma_speaker* pSpeaker)
{
    if (pSpeaker == NULL) {
        return;
    }

    ma_pcm_rb_reset(&pSpeaker->ringBuffer);
}

MA_WRAPPER_API void ma_microphone_flush(ma_microphone* pMicrophone)
{
    if (pMicrophone == NULL) {
        return;
    }

    ma_pcm_rb_reset(&pMicrophone->ringBuffer);
}
