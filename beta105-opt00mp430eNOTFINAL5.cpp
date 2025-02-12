//   lol  just encoded this baby - but i cant run it on this hardware - needs a nvenc 2000  or higher ... 
//
//            g++ -std=c++17 -o videoaudio125.exe videoaudio125.cpp     -I"C:/msys64/mingw64/include"     -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale     -lgdi32 -luser32
//    g++ -std=c++17 -o beta03.exe beta03.cpp     -I"C:/msys64/mingw64/include" -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale -lgdi32 -luser32 -lole32 -mconsole
//
//
//    g++ -std=c++17 -o beta10.exe beta10.cpp    -I"C:/NVIDIA_VideoCodecSDK_9.1/include"    -I"C:/ffmpeg/include"    -L"C:/msys64/mingw64/lib"    -L"C:/NVIDIA_VideoCodecSDK_9.1/lib"    -L"C:/ffmpeg/lib"    -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 -lole32    -lavcodec -lavformat -lavutil -lswscale    -static-libgcc -static-libstdc++ -mconsole
//
//





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Correct PTS calculation based on real-time timestamps
//frame->pts = av_rescale_q(elapsedTime, (AVRational){1, 1000000}, codecContext->time_base);     //  was using this  ?
//frame->pts = (int64_t)(frameCounter * (1000.0 / 30.0 * 44.77));//////////////////44.84/////81///44.78///





#include <chrono>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <ctime>

#include <audioclient.h>
#include <mmdeviceapi.h>
#pragma comment(lib, "Ole32.lib")

extern "C" {
    #include <libavutil/opt.h>  // Required for av_opt_set
}

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
    #include <libavutil/channel_layout.h>
}

#define FF_API_OLD_CHANNEL_LAYOUT 1

// Global variables
std::atomic<bool> isRecording(false);
std::atomic<bool> stopProgram(false);
std::mutex mtx;
std::condition_variable cv;

// -----------------------------------------------------------------------------
// Structure to hold captured frame data and its timestamp
struct CapturedFrame {
    uint8_t* data;
    int64_t timestamp; // in microseconds since start of capture
};

// -----------------------------------------------------------------------------
// FrameBuffer class to hold CapturedFrame pointers
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
// Capture the screen and store the pixel data in the provided buffer.
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
    bi.biHeight = -height;  // Negative for top-down bitmap
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(hDC, hBitmap, 0, height, buffer, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);

    return true;
}

// -----------------------------------------------------------------------------
// Initialize FFmpeg for MP4 output using the NVIDIA NVENC encoder.
bool initializeFFmpeg(AVFormatContext** formatContext, AVCodecContext** codecContext, int width, int height, const char* filename) {
    // Allocate format context for MP4
    avformat_alloc_output_context2(formatContext, nullptr, "mp4", filename);
    if (!*formatContext) {
        std::cerr << "Error: Could not allocate MP4 format context.\n";
        return false;
    }

    // Select NVIDIA NVENC Encoder
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        std::cerr << "Error: NVIDIA NVENC not found. Make sure your GPU drivers are installed.\n";
        return false;
    }

    // Allocate codec context
    *codecContext = avcodec_alloc_context3(codec);
    if (!*codecContext) {
        std::cerr << "Error: Could not allocate codec context.\n";
        return false;
    }

    (*codecContext)->width = width;
    (*codecContext)->height = height;
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    (*codecContext)->time_base = (AVRational){1, 1000000};  // 1/1,000,000 sec per tick (microsecond resolution)
    (*codecContext)->framerate = (AVRational){90, 1};         // 30 FPS
    (*codecContext)->pkt_timebase = (AVRational){1, 1000000};
    (*codecContext)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*codecContext)->gop_size = 90;   // Keyframe every 15 frames
    (*codecContext)->max_b_frames = 0;

    // NVENC requires global header flag
    (*codecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; 

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fps", "90", 0);
av_dict_set(&opts, "rc", "constqp", 0);
//    av_dict_set(&opts, "rc", "cbr", 0);
    av_dict_set(&opts, "vsync", "cfr", 0);
    av_dict_set(&opts, "force-cfr", "1", 0);
    av_dict_set(&opts, "framerate", "90000/1001", 0);
    av_dict_set(&opts, "preset", "p4", 0);
    av_dict_set(&opts, "tune", "hq", 0);

    av_dict_set(&opts, "cq", "17", 0);
    av_dict_set(&opts, "bitrate", "25000000", 0);   // 20 Mbps target
    av_dict_set(&opts, "maxrate", "25000000", 0);
    av_dict_set(&opts, "bufsize", "50000000", 0);
    av_dict_set(&opts, "delay", "0", 0);
    av_dict_set(&opts, "zerolatency", "1", 0);
    av_dict_set(&opts, "bf", "0", 0);
    av_dict_set(&opts, "gpu", "0", 0);
    av_dict_set(&opts, "threads", "auto", 0);
    av_dict_set(&opts, "g", "90", 0);
    av_dict_set(&opts, "temporal-aq", "1", 0);
    av_dict_set(&opts, "spatial-aq", "1", 0);
    av_dict_set(&opts, "aq-strength", "15", 0);
    av_dict_set(&opts, "profile", "100", 0);
    av_dict_set(&opts, "rc-lookahead", "20", 0);
    av_dict_set(&opts, "no-scenecut", "1", 0);
    av_dict_set(&opts, "strict_gop", "1", 0);

    if (avcodec_open2(*codecContext, codec, &opts) < 0) {
        std::cerr << "Error: Could not open NVENC codec.\n";
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    // Create new stream in the format context
    AVStream* stream = avformat_new_stream(*formatContext, nullptr);
    if (!stream) {
        std::cerr << "Error: Could not create stream.\n";
        return false;
    }
    avcodec_parameters_from_context(stream->codecpar, *codecContext);
    stream->time_base = (*codecContext)->time_base;
    stream->avg_frame_rate = (*codecContext)->framerate;

    // Open output file if needed
    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Could not open output MP4 file.\n";
            return false;
        }
    }

    // Write header
    if (avformat_write_header(*formatContext, nullptr) < 0) {
        std::cerr << "Error: Could not write MP4 header.\n";
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Encode a frame: convert the BGRA buffer (rgbBuffer) to YUV420P,
// set a presentation timestamp (PTS) based on real-time, and send it to the encoder.
void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext,
                 uint8_t* rgbBuffer, int width, int height,
                 int64_t& frameCounter,
                 std::chrono::high_resolution_clock::time_point startTime) {
    // Allocate and initialize an AVFrame
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "Error: Could not allocate frame.\n";
        return;
    }
    frame->format = codecContext->pix_fmt;
    frame->width  = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        std::cerr << "Error: Could not allocate frame data.\n";
        av_frame_free(&frame);
        return;
    }
    
    // Set PTS using real-time timestamps
    auto currentTime = std::chrono::high_resolution_clock::now();
    int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();
    frame->pts = av_rescale_q(elapsedTime, (AVRational){1, 1000000}, codecContext->time_base);

    // Convert the BGRA buffer to YUV420P
    SwsContext* swsContext = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                            width, height, AV_PIX_FMT_YUV420P,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsContext) {
        std::cerr << "Error: Could not create scaling context.\n";
        av_frame_free(&frame);
        return;
    }
    uint8_t* srcSlice[1] = { rgbBuffer };
    int srcStride[1] = { 4 * width };
    sws_scale(swsContext, srcSlice, srcStride, 0, height, frame->data, frame->linesize);
    sws_freeContext(swsContext);

    // Encode the frame
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
// The recordingThread handles starting the recording session:
// it initializes FFmpeg, starts a capture thread to grab screen frames,
// and an encoding thread to encode and write frames.
void recordingThread(int width, int height) {
    char outputFilename[128];
    time_t now = time(nullptr);
    strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mp4", localtime(&now));

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    if (!initializeFFmpeg(&formatContext, &codecContext, width, height, outputFilename)) {
        return;
    }

    // Create a FrameBuffer for captured frames (adjust buffer size as needed)
    FrameBuffer frameBuffer(30);

    int64_t localFrameCounter = 0;
    auto startTime = std::chrono::high_resolution_clock::now();

    // -------------------------------------------------------------------------
    // Capture Thread: Capture screen frames and push them into the buffer.
    std::thread captureThread([&]() {
        while (isRecording) {
            // Allocate a new buffer for each frame
            uint8_t* buffer = new uint8_t[width * height * 4];

            auto captureStart = std::chrono::high_resolution_clock::now();
            if (!captureScreen(buffer, width, height)) {
                std::cerr << "Error capturing screen.\n";
                delete[] buffer;
                break;
            }
            auto captureEnd = std::chrono::high_resolution_clock::now();
            int64_t captureDuration = std::chrono::duration_cast<std::chrono::microseconds>(captureEnd - captureStart).count();

            // Ensure a stable frame interval of 30 FPS-------------------------------------------------------------------------------------
            int64_t waitTime = (1000000 / 90) - captureDuration;
            if (waitTime > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
            }

            auto currentTime = std::chrono::high_resolution_clock::now();
            int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();

            // Create a CapturedFrame struct and push it to the buffer
            CapturedFrame* capFrame = new CapturedFrame;
            capFrame->data = buffer;
            capFrame->timestamp = elapsedTime;
            frameBuffer.push(capFrame);
        }
    });

    // -------------------------------------------------------------------------
    // Encoding Thread: Pop frames from the buffer and encode them.
    std::thread encodingThread([&]() {
        while (isRecording || !frameBuffer.isEmpty()) {
            CapturedFrame* capFrame = frameBuffer.pop();
            if (capFrame == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // Pass the startTime parameter to calculate PTS correctly
            encodeFrame(codecContext, formatContext, capFrame->data, width, height, localFrameCounter, startTime);
            delete[] capFrame->data;
            delete capFrame;
        }
    });

    captureThread.join();
    encodingThread.join();

    av_write_trailer(formatContext);
    avcodec_free_context(&codecContext);
    avformat_free_context(formatContext);

    std::cout << "Recording saved to " << outputFilename << ".\n";
}

// -----------------------------------------------------------------------------
// Low-level keyboard hook to toggle recording when the 'U' key is pressed.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;
        if (kbStruct->vkCode == 0x55) { // 'U' key
            if (wParam == WM_KEYDOWN) {
                isRecording = !isRecording;
                cv.notify_all();
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// -----------------------------------------------------------------------------
// Main function: Sets up the recording thread and the keyboard hook.
int main() {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);

    if (width <= 0 || height <= 0) {
        std::cerr << "Error: Invalid screen resolution.\n";
        return -1;
    }

    std::thread recorder([&]() {
        while (!stopProgram) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, []() { return isRecording || stopProgram; });
            if (stopProgram) break;

            std::cout << "Recording started. Press U to stop.\n";
            recordingThread(width, height);
            std::cout << "Recording stopped. Press U to start again.\n";
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











