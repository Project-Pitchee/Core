#include "mac_recorder.hpp"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <thread>

bool record_macos_audio(
    const std::string& output_path,
    double duration_seconds,
    std::string& error
) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:output_path.c_str()];
        if (path == nil) {
            error = "recording path is not valid UTF-8";
            return false;
        }
        NSURL* url = [NSURL fileURLWithPath:path];
        AVAudioEngine* engine = [[AVAudioEngine alloc] init];
        AVAudioInputNode* input = [engine inputNode];
        AVAudioFormat* input_format = [input outputFormatForBus:0];
        if (input_format == nil || input_format.sampleRate <= 0.0
            || input_format.channelCount == 0) {
            error = "macOS audio input is unavailable";
            return false;
        }

        NSDictionary* settings = @{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVSampleRateKey: @(input_format.sampleRate),
            AVNumberOfChannelsKey: @(input_format.channelCount),
            AVLinearPCMBitDepthKey: @32,
            AVLinearPCMIsFloatKey: @YES,
            AVLinearPCMIsBigEndianKey: @NO,
            AVLinearPCMIsNonInterleaved: @NO,
        };
        NSError* native_error = nil;
        AVAudioFile* file = [[AVAudioFile alloc]
            initForWriting:url
            settings:settings
            commonFormat:AVAudioPCMFormatFloat32
            interleaved:NO
            error:&native_error];
        if (file == nil) {
            error = native_error != nil
                ? [[native_error localizedDescription] UTF8String]
                : "unable to create recording file";
            return false;
        }

        __block NSString* write_error = nil;
        [input installTapOnBus:0
            bufferSize:4096
            format:input_format
            block:^(AVAudioPCMBuffer* buffer, AVAudioTime* when) {
                (void)when;
                NSError* current_error = nil;
                if (![file writeFromBuffer:buffer error:&current_error]
                    && write_error == nil) {
                    write_error = [current_error localizedDescription];
                }
            }];

        [engine prepare];
        if (![engine startAndReturnError:&native_error]) {
            [input removeTapOnBus:0];
            error = native_error != nil
                ? [[native_error localizedDescription] UTF8String]
                : "unable to start macOS audio input";
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(duration_seconds)
            );
        while (std::chrono::steady_clock::now() < deadline && write_error == nil) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        [engine stop];
        [input removeTapOnBus:0];
        if (@available(macOS 15.0, *)) {
            [file close];
        }
        file = nil;
        if (write_error != nil) {
            error = [write_error UTF8String];
            return false;
        }
        return true;
    }
}
