#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <stdio.h>
#include <stdlib.h>

static const int kWidth = 320;
static const int kHeight = 240;
static const int kFps = 10;
static const int kFrameCount = 40;
static NSMutableArray<NSData *> *s_frames;
static NSLock *s_frame_lock;
static OSStatus s_encode_error;

static void append_start_code(NSMutableData *data)
{
    static const uint8_t kStartCode[] = {0, 0, 0, 1};
    [data appendBytes:kStartCode length:sizeof(kStartCode)];
}

static BOOL append_parameter_set(NSMutableData *data, CMFormatDescriptionRef format, size_t index)
{
    const uint8_t *bytes = NULL;
    size_t size = 0;
    OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        format, index, &bytes, &size, NULL, NULL);
    if (status != noErr || bytes == NULL || size == 0) {
        return NO;
    }
    append_start_code(data);
    [data appendBytes:bytes length:size];
    return YES;
}

static BOOL append_annex_b(NSMutableData *data, const uint8_t *bytes, size_t size,
                           size_t length_size)
{
    if (length_size == 0 || length_size > 4) {
        return NO;
    }
    size_t offset = 0;
    while (offset < size) {
        if (size - offset < length_size) {
            return NO;
        }
        uint32_t nal_size = 0;
        for (size_t i = 0; i < length_size; i++) {
            nal_size = (nal_size << 8) | bytes[offset + i];
        }
        offset += length_size;
        if (nal_size == 0 || nal_size > size - offset) {
            return NO;
        }
        append_start_code(data);
        [data appendBytes:bytes + offset length:nal_size];
        offset += nal_size;
    }
    return YES;
}

static void compression_callback(void *output_refcon, void *source_refcon,
                                 OSStatus status, VTEncodeInfoFlags flags,
                                 CMSampleBufferRef sample)
{
    (void)output_refcon;
    (void)source_refcon;
    (void)flags;
    @autoreleasepool {
        if (status != noErr || sample == NULL || !CMSampleBufferDataIsReady(sample)) {
            s_encode_error = status != noErr ? status : -1;
            return;
        }

        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        if (block == NULL || format == NULL) {
            s_encode_error = -2;
            return;
        }

        size_t byte_count = CMBlockBufferGetDataLength(block);
        NSMutableData *avcc = [NSMutableData dataWithLength:byte_count];
        if (CMBlockBufferCopyDataBytes(block, 0, byte_count, avcc.mutableBytes) != kCMBlockBufferNoErr) {
            s_encode_error = -3;
            return;
        }

        int nal_length_size = 0;
        size_t parameter_count = 0;
        OSStatus parameter_status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, 0, NULL, NULL, &parameter_count, &nal_length_size);
        if (parameter_status != noErr || parameter_count < 2 || nal_length_size < 1 || nal_length_size > 4) {
            s_encode_error = parameter_status != noErr ? parameter_status : -4;
            return;
        }

        NSMutableData *annex_b = [NSMutableData dataWithCapacity:byte_count + 64];
        if (!append_parameter_set(annex_b, format, 0) ||
            !append_parameter_set(annex_b, format, 1) ||
            !append_annex_b(annex_b, avcc.bytes, avcc.length, (size_t)nal_length_size) ||
            annex_b.length == 0 || annex_b.length > UINT32_MAX) {
            s_encode_error = -5;
            return;
        }

        [s_frame_lock lock];
        [s_frames addObject:annex_b];
        [s_frame_lock unlock];
    }
}

static void draw_frame(CVPixelBufferRef pixel_buffer, int frame_index)
{
    CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    uint8_t *base = CVPixelBufferGetBaseAddress(pixel_buffer);
    size_t stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    int moving_x = (frame_index * 7) % (kWidth - 42);
    for (int y = 0; y < kHeight; y++) {
        uint8_t *row = base + y * stride;
        for (int x = 0; x < kWidth; x++) {
            BOOL grid = (x % 40 == 0) || (y % 40 == 0);
            BOOL marker = x >= moving_x && x < moving_x + 42 && y >= 92 && y < 142;
            uint8_t blue = grid ? 52 : (uint8_t)(14 + (x * 22 / kWidth));
            uint8_t green = grid ? 78 : (uint8_t)(28 + (y * 36 / kHeight));
            uint8_t red = marker ? 226 : (uint8_t)(26 + ((x + y) % 18));
            row[x * 4 + 0] = blue;
            row[x * 4 + 1] = green;
            row[x * 4 + 2] = red;
            row[x * 4 + 3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
}

static int write_stream(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        perror("fopen output");
        return 1;
    }

    uint64_t total_bytes = 0;
    for (NSData *frame in s_frames) {
        uint32_t size = (uint32_t)frame.length;
        uint8_t length[4] = {
            (uint8_t)(size >> 24), (uint8_t)(size >> 16),
            (uint8_t)(size >> 8), (uint8_t)size,
        };
        if (fwrite(length, sizeof(length), 1, file) != 1 ||
            fwrite(frame.bytes, size, 1, file) != 1) {
            perror("fwrite output");
            fclose(file);
            return 1;
        }
        total_bytes += sizeof(length) + size;
    }
    if (fclose(file) != 0) {
        perror("fclose output");
        return 1;
    }
    fprintf(stderr, "generated %lu H.264 Annex-B access units, %llu bytes, %dx%d@%d\n",
            (unsigned long)s_frames.count, (unsigned long long)total_bytes,
            kWidth, kHeight, kFps);
    return s_frames.count == kFrameCount ? 0 : 1;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *output = argc > 1 ? argv[1] : "components/media_bridge/test_stream.bin";
        s_frames = [NSMutableArray arrayWithCapacity:kFrameCount];
        s_frame_lock = [[NSLock alloc] init];
        s_encode_error = noErr;

        VTCompressionSessionRef session = NULL;
        NSDictionary *pixel_attributes = @{
            (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (__bridge NSString *)kCVPixelBufferWidthKey: @(kWidth),
            (__bridge NSString *)kCVPixelBufferHeightKey: @(kHeight),
            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault, kWidth, kHeight, kCMVideoCodecType_H264,
            NULL, (__bridge CFDictionaryRef)pixel_attributes, NULL,
            compression_callback, NULL, &session);
        if (status != noErr || session == NULL) {
            fprintf(stderr, "VTCompressionSessionCreate failed: %d\n", (int)status);
            return 1;
        }

        VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_Baseline_AutoLevel);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate,
                             (__bridge CFTypeRef)@(kFps));
        VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                             (__bridge CFTypeRef)@(kFps));
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate,
                             (__bridge CFTypeRef)@(160000));
        status = VTCompressionSessionPrepareToEncodeFrames(session);
        if (status != noErr) {
            fprintf(stderr, "VTCompressionSessionPrepareToEncodeFrames failed: %d\n", (int)status);
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
            return 1;
        }

        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
        if (pool == NULL) {
            fprintf(stderr, "VideoToolbox did not create a pixel buffer pool\n");
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
            return 1;
        }
        for (int frame_index = 0; frame_index < kFrameCount; frame_index++) {
            CVPixelBufferRef pixel_buffer = NULL;
            status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixel_buffer);
            if (status != kCVReturnSuccess || pixel_buffer == NULL) {
                fprintf(stderr, "CVPixelBufferPoolCreatePixelBuffer failed: %d\n", (int)status);
                break;
            }
            draw_frame(pixel_buffer, frame_index);
            CMTime pts = CMTimeMake(frame_index, kFps);
            CMTime duration = CMTimeMake(1, kFps);
            NSDictionary *properties = frame_index == 0
                ? @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES}
                : nil;
            VTEncodeInfoFlags flags = 0;
            status = VTCompressionSessionEncodeFrame(session, pixel_buffer, pts, duration,
                                                      (__bridge CFDictionaryRef)properties,
                                                      NULL, &flags);
            CVPixelBufferRelease(pixel_buffer);
            if (status != noErr) {
                fprintf(stderr, "VTCompressionSessionEncodeFrame failed: %d\n", (int)status);
                break;
            }
        }

        OSStatus flush_status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        if (status != noErr || flush_status != noErr || s_encode_error != noErr) {
            fprintf(stderr, "encode failed: status=%d flush=%d callback=%d\n",
                    (int)status, (int)flush_status, (int)s_encode_error);
            return 1;
        }
        return write_stream(output);
    }
}
