

//            g++ -std=c++17 -o videoaudio125.exe videoaudio125.cpp     -I"C:/msys64/mingw64/include"     -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale     -lgdi32 -luser32
//    g++ -std=c++17 -o beta03.exe beta03.cpp     -I"C:/msys64/mingw64/include" -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale -lgdi32 -luser32 -lole32 -mconsole
//            g++ -std=c++17 -o videoaudio125.exe videoaudio125.cpp     -I"C:/msys64/mingw64/include"     -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale     -lgdi32 -luser32
//    g++ -std=c++17 -o beta03.exe beta03.cpp     -I"C:/msys64/mingw64/include" -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale -lgdi32 -luser32 -lole32 -mconsole
#include <chrono>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>  // ✅ Fixes std::condition_variable error
#include <memory>              // ✅ Fixes smart pointers

#include <audioclient.h>
#include <mmdeviceapi.h>
#pragma comment(lib, "Ole32.lib")

#include <condition_variable>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
}


#define FF_API_OLD_CHANNEL_LAYOUT 1

std::atomic<bool> isRecording(false);
std::atomic<bool> stopProgram(false);
std::mutex mtx;
std::condition_variable cv;


// -----------------------------------------------------------------------------
// Added structure to hold captured frame data and its timestamp for encoding
struct CapturedFrame {
    uint8_t* data;
    int64_t timestamp; // in microseconds since start of capture
};



// -----------------------------------------------------------------------------
// Modified FrameBuffer to hold CapturedFrame pointers instead of raw uint8_t*
// (All original comments and formatting preserved)
class FrameBuffer {
private:
    std::queue<CapturedFrame*> bufferQueue;
    size_t maxSize;
    std::mutex mtx;

public:
    FrameBuffer(size_t size) : maxSize(size) {}

    void push(CapturedFrame* frame) {
        std::lock_guard<std::mutex> lock(mtx);
        if (bufferQueue.size() >= maxSize) {
            CapturedFrame* oldFrame = bufferQueue.front();
            delete[] oldFrame->data; // Drop the oldest frame
            delete oldFrame;
            bufferQueue.pop();
        }
        bufferQueue.push(frame);
    }

    CapturedFrame* pop() {
        std::lock_guard<std::mutex> lock(mtx);
        if (bufferQueue.empty()) return nullptr;
        CapturedFrame* frame = bufferQueue.front();
        bufferQueue.pop();
        return frame;
    }

    bool isEmpty() {
        std::lock_guard<std::mutex> lock(mtx);
        return bufferQueue.empty();
    }
};




// -----------------------------------------------------------------------------
bool captureScreen(uint8_t* buffer, int width, int height) {
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, width, height);
    SelectObject(hDC, hBitmap);

    if (!BitBlt(hDC, 0, 0, width, height, hScreen, 0, 0, SRCCOPY)) {
        std::cerr << "Error: BitBlt failed.\n";
        DeleteObject(hBitmap);
        DeleteDC(hDC);
        ReleaseDC(NULL, hScreen);
        return false;
    }

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(hDC, hBitmap, 0, height, buffer, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
//formatContext->streams[0]->avg_frame_rate = {60, 1};

    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);

    return true;
}



// -----------------------------------------------------------------------------
bool initializeFFmpeg(AVFormatContext** formatContext, AVCodecContext** codecContext, int width, int height, const char* filename) {
    avformat_alloc_output_context2(formatContext, nullptr, nullptr, filename);
    if (!*formatContext) {
        std::cerr << "Error: Could not allocate output context.\n";
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        std::cerr << "Error: Codec not found.\n";
        return false;
    }

    *codecContext = avcodec_alloc_context3(codec);
    if (!*codecContext) {
        std::cerr << "Error: Could not allocate codec context.\n";
        return false;
    }
 AVDictionary* opts = nullptr;
    (*codecContext)->width = width;
    (*codecContext)->height = height;
    (*codecContext)->time_base = {1, 1000000}; // Microseconds for precise timing
    (*codecContext)->framerate = {30, 1};      // Desired FPS (30)
    (*codecContext)->pix_fmt = AV_PIX_FMT_YUV420P;
//    (*codecContext)->gop_size = 60; // Keyframe every second (30 FPS)
    (*codecContext)->max_b_frames = 0;





av_dict_set(&opts, "vsync", "cfr", 0);  // Force constant frame rate
av_dict_set(&opts, "tune", "llhq", 0);
    av_dict_set(&opts, "force-cfr", "1", 0); // Force Constant Frame Rate mode
//    av_dict_set(&opts, "vsync", "1", 0);     // Synchronize frames to prevent drift
av_dict_set(&opts, "framerate", "30000/1001", 0); ////   what   dictate framerates    yes ?
    av_dict_set(&opts, "preset", "p4", 0); // "p4" = Performance-focused preset
//    av_dict_set(&opts, "tune", "hq", 0);   // High-quality tuning
   av_dict_set(&opts, "rc", "cbr", 0);    // Variable bitrate (uncomment if needed) --- wrong
//    av_dict_set(&opts, "rc", "cqp", 0);      // Use constant quantizer mode instead of cbr
    av_dict_set(&opts, "cq", "21", 0);
//    av_dict_set(&opts, "rc", "constqp", 0);
    av_dict_set(&opts, "bitrate", "15000000", 0);  // 20 Mbps bitrate target
    av_dict_set(&opts, "maxrate", "15000000", 0);  // 20 Mbps max bitrate
    av_dict_set(&opts, "bufsize", "30000000", 0);  // Buffer size for VBR stability
    av_dict_set(&opts, "delay", "0", 0);
av_dict_set(&opts, "b_adapt", "0", 0); 
    av_dict_set(&opts, "zerolatency", "1", 0);  // Ensures minimal encoding delay
    av_dict_set(&opts, "bf", "0", 0);           // No B-frames for low latency
    av_dict_set(&opts, "gpu", "0", 0);          // Ensure it's using the correct GPU
    av_dict_set(&opts, "threads", "auto", 0);   // Let NVENC handle threading
    av_dict_set(&opts, "g", "1", 0);           // Keyframe interval: 1s (for smooth seeking)
    av_dict_set(&opts, "temporal-aq", "1", 0);   // Adaptive quantization for better motion quality
    av_dict_set(&opts, "spatial-aq", "1", 0);
    av_dict_set(&opts, "aq-strength", "10", 0);   // Moderate AQ strength
//    av_dict_set(&opts, "profile", "high", 0);
   av_dict_set(&opts, "profile", "100", 0); // Set high profile (100) for NVENC
    av_dict_set(&opts, "rc-lookahead", "8", 0);  // Optimizes frame prediction
av_dict_set(&opts, "no-scenecut", "1", 0);  // Prevent NVENC from skipping frames
av_dict_set(&opts, "strict_gop", "1", 0);   // Ensure frames stay at 30 FPS
av_dict_set(&opts, "rc", "cbr_hq", 0);  // Force high-quality CBR
//av_dict_set(&opts, "refs", "4", 0); // Use 4 reference frames for better quality
av_dict_set(&opts, "lookahead", "40", 0);  // Increase lookahead frames for better quality
av_dict_set(&opts, "nonref_p", "1", 0);  // Use non-reference P-frames for better efficiency
av_dict_set(&opts, "repeat-headers", "1", 0);  // Ensure keyframe headers for seamless seeking
av_dict_set(&opts, "aud", "1", 0);  // Include Access Unit Delimiters (AUD) for decoder sync
av_dict_set(&opts, "sei", "1", 0);  // Enable SEI metadata to improve frame decoding
av_dict_set(&opts, "weighted_pred", "1", 0);  // Enable weighted prediction for better interpolation
av_dict_set(&opts, "sc_threshold", "40", 0);  // Reduce scene-cut threshold for smoother transitions
av_dict_set(&opts, "motion-est", "medium", 0);  // Reduces motion estimation workload
av_dict_set(&opts, "multipass", "quarterres", 0);  // Reduces computation by lowering mult
av_dict_set(&opts, "forced-idr", "1", 0); 


/*




Rate Control & Bitrate Settings
cpp
Copy
Edit
av_dict_set(&opts, "rc", "vbr", 0);  // Variable bitrate mode
av_dict_set(&opts, "cbr", "1", 0);   // Constant bitrate mode (alternative to rc=cbr)
av_dict_set(&opts, "qp", "23", 0);   // Set quantization parameter for constant QP mode
av_dict_set(&opts, "cq", "18", 0);   // Constant quality mode (lower is better)
av_dict_set(&opts, "maxrate", "25000000", 0);  // Set maximum bitrate (25 Mbps)
av_dict_set(&opts, "minrate", "5000000", 0);   // Set minimum bitrate (5 Mbps)
av_dict_set(&opts, "bufsize", "50000000", 0);  // Increase buffer size for smoother bitrate fluctuations
av_dict_set(&opts, "lookahead", "32", 0);  // Increase lookahead frames for better quality
Performance & Encoding Efficiency
cpp
Copy
Edit
av_dict_set(&opts, "preset", "p7", 0);  // Use the highest-quality preset for NVENC
av_dict_set(&opts, "preset", "slow", 0);  // Alternative slower encoding for better quality
av_dict_set(&opts, "tune", "hq", 0);  // High-quality tuning
av_dict_set(&opts, "tune", "ll", 0);  // Low-latency tuning (for real-time applications)
av_dict_set(&opts, "delay", "0", 0);  // Eliminate encoder buffering delay
av_dict_set(&opts, "zerolatency", "1", 0);  // Forces low-latency encoding
av_dict_set(&opts, "gpu", "1", 0);  // Force a specific GPU for encoding
av_dict_set(&opts, "forced-idr", "1", 0);  // Force IDR frames at keyframe intervals
av_dict_set(&opts, "strict_gop", "1", 0);  // Ensure GOP structure remains fixed
av_dict_set(&opts, "nonref_p", "1", 0);  // Use non-reference P-frames for better efficiency
av_dict_set(&opts, "b_adapt", "0", 0);  // Disable adaptive B-frames for consistent latency
av_dict_set(&opts, "spatial-aq", "1", 0);  // Enable spatial adaptive quantization
av_dict_set(&opts, "temporal-aq", "1", 0);  // Enable temporal adaptive quantization
av_dict_set(&opts, "aq-strength", "10", 0);  // Adjust adaptive quantization strength
av_dict_set(&opts, "mbtree", "1", 0);  // Enable macroblock tree-based rate control
av_dict_set(&opts, "b-pyramid", "0", 0);  // Disable B-frame pyramid to reduce latency
Keyframe & GOP Settings
cpp
Copy
Edit
av_dict_set(&opts, "g", "120", 0);  // Keyframe interval (higher for better compression, lower for low-latency)
av_dict_set(&opts, "bf", "2", 0);   // Allow up to 2 B-frames for better compression
av_dict_set(&opts, "bf", "0", 0);   // Disable B-frames for minimal latency
av_dict_set(&opts, "refs", "4", 0); // Use 4 reference frames for better quality
av_dict_set(&opts, "no-scenecut", "1", 0);  // Prevent encoder from skipping frames at scene changes
av_dict_set(&opts, "forced-idr", "1", 0);  // Always use IDR frames at GOP boundaries
Tuning for Streaming & Low Latency
cpp
Copy
Edit
av_dict_set(&opts, "tune", "llhp", 0);  // Low-latency high-performance mode
av_dict_set(&opts, "tune", "llhq", 0);  // Low-latency high-quality mode
av_dict_set(&opts, "repeat-headers", "1", 0);  // Send keyframe headers with each keyframe (important for streaming)
av_dict_set(&opts, "aud", "1", 0);  // Include Access Unit Delimiter (AUD) for compatibility with certain players
av_dict_set(&opts, "sei", "1", 0);  // Enable SEI metadata for better synchronization
av_dict_set(&opts, "forced-idr", "1", 0);  // Force IDR frame insertion for better seekability
Advanced NVENC Settings
cpp
Copy
Edit
av_dict_set(&opts, "multipass", "fullres", 0);  // Enable full-resolution multi-pass encoding for quality
av_dict_set(&opts, "multipass", "quarterres", 0);  // Lower-quality multi-pass mode for speed
av_dict_set(&opts, "enable-ltr", "1", 0);  // Enable long-term reference frames for better quality
av_dict_set(&opts, "weighted_pred", "1", 0);  // Enable weighted P-frame prediction
av_dict_set(&opts, "weighted_bipred", "1", 0);  // Enable weighted B-frame prediction
av_dict_set(&opts, "motion-est", "high", 0);  // High-quality motion estimation
av_dict_set(&opts, "sc_threshold", "40", 0);  // Scene change detection threshold
av_dict_set(&opts, "rc-lookahead", "32", 0);  // Lookahead for rate control
av_dict_set(&opts, "enable-dyn-b", "1", 0);  // Enable dynamic B-frame placement



*/












    if (avcodec_open2(*codecContext, codec, nullptr) < 0) {
        std::cerr << "Error: Could not open codec.\n";
        return false;
    }

    AVStream* stream = avformat_new_stream(*formatContext, nullptr);
    if (!stream) {
        std::cerr << "Error: Could not create stream.\n";
        return false;
    }

    avcodec_parameters_from_context(stream->codecpar, *codecContext);
    stream->time_base = (*codecContext)->time_base; // Match codec time_base
    stream->avg_frame_rate = (*codecContext)->framerate;

    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Could not open output file.\n";
            return false;
        }
    }

    if (avformat_write_header(*formatContext, nullptr) < 0) {
        std::cerr << "Error: Could not write header.\n";
        return false;
    }

    return true;
}



// -----------------------------------------------------------------------------
void captureAudio(IAudioClient* audioClient, IAudioCaptureClient* captureClient, AVFormatContext* formatContext, AVCodecContext* codecContext) {
    while (isRecording) {
        UINT32 packetLength = 0;
        if (captureClient->GetNextPacketSize(&packetLength) == S_OK && packetLength > 0) {
            BYTE* data;
            DWORD flags;
            UINT32 numFramesAvailable;
            if (captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr) == S_OK) {
                // Encode audio data
                AVFrame* frame = av_frame_alloc();
                frame->nb_samples = numFramesAvailable;
                frame->format = codecContext->sample_fmt;
//                frame->channel_layout = codecContext->channel_layout;
frame->ch_layout = codecContext->ch_layout;
                frame->sample_rate = codecContext->sample_rate;

//                avcodec_fill_audio_frame(frame, codecContext->channels, codecContext->sample_fmt, data, numFramesAvailable * 4, 0);
avcodec_fill_audio_frame(frame, av_channel_layout_check(&codecContext->ch_layout), 
                         codecContext->sample_fmt, data, numFramesAvailable * 4, 0);

                frame->pts = av_rescale_q(numFramesAvailable, {1, codecContext->sample_rate}, codecContext->time_base);

                AVPacket* packet = av_packet_alloc();
                if (avcodec_send_frame(codecContext, frame) >= 0) {
                    while (avcodec_receive_packet(codecContext, packet) >= 0) {
                        av_interleaved_write_frame(formatContext, packet);
                        av_packet_unref(packet);
                    }
                }

                av_frame_free(&frame);
                av_packet_free(&packet);
                captureClient->ReleaseBuffer(numFramesAvailable);
            }
        }
    }

    audioClient->Stop();
}



// -----------------------------------------------------------------------------
void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext, uint8_t* rgbBuffer, int width, int height, int64_t& frameCounter, int64_t elapsedTime) {
    AVFrame* frame = av_frame_alloc();
    frame->format = codecContext->pix_fmt;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 32);


//width = (width / 2) * 2;      //   code to fix odd screen size -might not be in the correct location
//height = (height / 2) * 2;

    SwsContext* swsContext = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    uint8_t* srcSlice[1] = {rgbBuffer};
    int srcStride[1] = {4 * width};
    sws_scale(swsContext, srcSlice, srcStride, 0, height, frame->data, frame->linesize);
    sws_freeContext(swsContext);

    // Calculate PTS from elapsed time (scaled to time_base)
    frame->pts = elapsedTime * codecContext->time_base.den / 1000000;

    std::cout << "Frame: " << frameCounter << " | PTS: " << frame->pts << " | Elapsed Time: " << elapsedTime << " µs\n";

    frameCounter++;

    AVPacket* packet = av_packet_alloc();
    if (avcodec_send_frame(codecContext, frame) >= 0) {
        while (avcodec_receive_packet(codecContext, packet) >= 0) {
            av_interleaved_write_frame(formatContext, packet);
            av_packet_unref(packet);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
}



// -----------------------------------------------------------------------------
#include <chrono>

void recordingThread(int width, int height) {
    char outputFilename[128];
    time_t now = time(nullptr);
    strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mp4", localtime(&now));
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;

    if (!initializeFFmpeg(&formatContext, &codecContext, width, height, outputFilename)) {
        return;
    }

    // Create a FrameBuffer for captured frames (buffer size can be tuned as needed)
    FrameBuffer frameBuffer(30);

    int64_t frameCounter = 0;
    auto startTime = std::chrono::high_resolution_clock::now();

    // -------------------------------------------------------------------------
    // Capture Thread: Captures screen frames and pushes them into the buffer.
    std::thread captureThread([&]() {
        auto nextFrameTime = startTime;
        while (isRecording) {
            // Allocate a new buffer for each frame
            uint8_t* buffer = new uint8_t[width * height * 4];

            // Capture screen into the new buffer and record capture timestamp
            auto captureStart = std::chrono::high_resolution_clock::now();
            if (!captureScreen(buffer, width, height)) {
                std::cerr << "Error capturing screen.\n";
                delete[] buffer;
                break;
            }
            auto captureEnd = std::chrono::high_resolution_clock::now();
            int64_t captureTime = std::chrono::duration_cast<std::chrono::microseconds>(captureEnd - captureStart).count();
            std::cout << "Capture Time: " << captureTime << " µs\n";

            auto currentTime = std::chrono::high_resolution_clock::now();
            int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();

            // Create a CapturedFrame struct and push it to the buffer
            CapturedFrame* capFrame = new CapturedFrame;
            capFrame->data = buffer;
            capFrame->timestamp = elapsedTime;
            frameBuffer.push(capFrame);

            // Maintain steady FPS (60 FPS = ~16,666 µs per frame)
            nextFrameTime += std::chrono::microseconds(1000000 / 60);
            std::this_thread::sleep_until(nextFrameTime);
        }
    });

    // -------------------------------------------------------------------------
    // Encoding Thread: Pops frames from the buffer and encodes them.
    std::thread encodingThread([&]() {
        // Continue processing while recording or there are frames in the buffer
        while (isRecording || !frameBuffer.isEmpty()) {
            CapturedFrame* capFrame = frameBuffer.pop();
            if (capFrame == nullptr) {
                // If no frame is available, sleep briefly and try again.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // Measure encoding time
            auto encodeStart = std::chrono::high_resolution_clock::now();
            encodeFrame(codecContext, formatContext, capFrame->data, width, height, frameCounter, capFrame->timestamp);
            auto encodeEnd = std::chrono::high_resolution_clock::now();
            int64_t encodeTime = std::chrono::duration_cast<std::chrono::microseconds>(encodeEnd - encodeStart).count();
            std::cout << "Encoding Time: " << encodeTime << " µs\n";

            // Free the frame data after encoding
            delete[] capFrame->data;
            delete capFrame;
        }
    });

    // Wait for both threads to finish
    captureThread.join();
    encodingThread.join();

    av_write_trailer(formatContext);
    avcodec_free_context(&codecContext);
    avformat_free_context(formatContext);

    std::cout << "Recording saved to " << outputFilename << ".\n";
}



// -----------------------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;
        if (kbStruct->vkCode == VK_OEM_5) { // '\' key
            if (wParam == WM_KEYDOWN) {
                isRecording = !isRecording;
                cv.notify_all();
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int main() {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
//
//width = (width / 2) * 2;      //   code to fix odd screen size -might not be in the correct location
//height = (height / 2) * 2;
//
    if (width <= 0 || height <= 0) {
        std::cerr << "Error: Invalid screen resolution.\n";
        return -1;
    }

    std::thread recorder([&]() {
        while (!stopProgram) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, []() { return isRecording || stopProgram; });
            if (stopProgram) break;

            std::cout << "Recording started. Press \\\\ to stop.\n";
            recordingThread(width, height);
            std::cout << "Recording stopped. Press \\\\ to start again.\n";
        }
    });

    HHOOK keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!keyboardHook) {
        std::cerr << "Error: Could not set keyboard hook.\n";
        return -1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    stopProgram = true;
    cv.notify_all();
    recorder.join();

    UnhookWindowsHookEx(keyboardHook);
    return 0;
}
