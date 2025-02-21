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
#include <d3d11.h>
#include <dxgi1_2.h>
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
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

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

// Global control variables
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
// FrameBuffer class to hold CapturedFrame pointers in a thread-safe manner
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
// Global variables for Desktop Duplication API objects
ID3D11Device*           g_d3dDevice     = nullptr;
ID3D11DeviceContext*    g_d3dContext    = nullptr;
IDXGIOutputDuplication* g_duplication   = nullptr;
ID3D11Texture2D*        g_stagingTexture= nullptr;

// -----------------------------------------------------------------------------
// Initialize Desktop Duplication for fast screen capture using DirectX 11
bool initializeDesktopDuplication(int width, int height) {
    HRESULT hr;

    // Create D3D11 device and context
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                           nullptr, 0, D3D11_SDK_VERSION, &g_d3dDevice, nullptr, &g_d3dContext);
    if (FAILED(hr)) {
        std::cerr << "Error: D3D11CreateDevice failed.\n";
        return false;
    }

    // Get DXGI device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        std::cerr << "Error: QueryInterface for IDXGIDevice failed.\n";
        return false;
    }

    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: GetParent for IDXGIAdapter failed.\n";
        return false;
    }

    // Get the first output (monitor)
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: EnumOutputs failed.\n";
        return false;
    }

    // Query for IDXGIOutput1 interface
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: QueryInterface for IDXGIOutput1 failed.\n";
        return false;
    }

    // Duplicate the output
    hr = dxgiOutput1->DuplicateOutput(g_d3dDevice, &g_duplication);
    dxgiOutput1->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: DuplicateOutput failed. Ensure your system supports Desktop Duplication API.\n";
        return false;
    }

    // Create a staging texture for CPU read access
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Common screen capture format
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_d3dDevice->CreateTexture2D(&desc, nullptr, &g_stagingTexture);
    if (FAILED(hr)) {
        std::cerr << "Error: CreateTexture2D for staging texture failed.\n";
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Release Desktop Duplication resources
void releaseDesktopDuplication() {
    if (g_stagingTexture) {
        g_stagingTexture->Release();
        g_stagingTexture = nullptr;
    }
    if (g_duplication) {
        g_duplication->Release();
        g_duplication = nullptr;
    }
    if (g_d3dContext) {
        g_d3dContext->Release();
        g_d3dContext = nullptr;
    }
    if (g_d3dDevice) {
        g_d3dDevice->Release();
        g_d3dDevice = nullptr;
    }
}

// -----------------------------------------------------------------------------
// Capture the screen using the Desktop Duplication API.
// The captured data is copied into the provided preallocated buffer (BGRA, 4 bytes per pixel).
bool captureScreenDXGI(uint8_t* buffer, int width, int height) {
    if (!g_duplication) {
        std::cerr << "Error: Desktop Duplication not initialized.\n";
        return false;
    }
    
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = g_duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        // Timeout or error – simply return false; you may choose to log or ignore timeout errors.
        return false;
    }
    
    // Get the acquired frame as a texture
    ID3D11Texture2D* acquiredDesktopImage = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&acquiredDesktopImage);
    desktopResource->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: QueryInterface for acquired frame failed.\n";
        g_duplication->ReleaseFrame();
        return false;
    }
    
    // Copy the frame to our staging texture so we can read it from the CPU
    g_d3dContext->CopyResource(g_stagingTexture, acquiredDesktopImage);
    acquiredDesktopImage->Release();
    
    // Map the staging texture to access the pixel data
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_d3dContext->Map(g_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Error: Mapping staging texture failed.\n";
        g_duplication->ReleaseFrame();
        return false;
    }
    
    // Copy each row from the mapped texture to our buffer
    for (int y = 0; y < height; y++) {
        memcpy(buffer + y * width * 4, (uint8_t*)mapped.pData + y * mapped.RowPitch, width * 4);
    }
    
    g_d3dContext->Unmap(g_stagingTexture, 0);
    g_duplication->ReleaseFrame();
    
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
    // Set time base and frame rate (using microsecond resolution)
    (*codecContext)->time_base = (AVRational){1, 1000000};
    (*codecContext)->framerate = (AVRational){60, 1};
    (*codecContext)->pkt_timebase = (AVRational){1, 1000000};

    (*codecContext)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*codecContext)->gop_size = 120;   
    (*codecContext)->max_b_frames = 0;
    (*codecContext)->thread_count = 7;

    // NVENC requires the global header flag
    (*codecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; 

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fps", "60", 0);
    av_dict_set(&opts, "rc", "cbr", 0);
    av_dict_set(&opts, "force-cfr", "1", 0);
    av_dict_set(&opts, "rc", "constqp", 0);
    av_dict_set(&opts, "framerate", "60000/1001", 0);
    av_dict_set(&opts, "preset", "p4", 0);
    av_dict_set(&opts, "tune", "hq", 0);
    av_dict_set(&opts, "cq", "18", 0);
    av_dict_set(&opts, "bitrate", "25000000", 0);
    av_dict_set(&opts, "maxrate", "25000000", 0);
    av_dict_set(&opts, "bufsize", "50000000", 0);
    av_dict_set(&opts, "delay", "0", 0);
    av_dict_set(&opts, "zerolatency", "1", 0);
    av_dict_set(&opts, "bf", "0", 0);
    av_dict_set(&opts, "gpu", "0", 0);
    av_dict_set(&opts, "threads", "auto", 0);
    av_dict_set(&opts, "g", "15", 0);
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

    // Create a new stream in the format context
    AVStream* stream = avformat_new_stream(*formatContext, nullptr);
    if (!stream) {
        std::cerr << "Error: Could not create stream.\n";
        return false;
    }
    avcodec_parameters_from_context(stream->codecpar, *codecContext);
    stream->time_base = (*codecContext)->time_base;
    stream->avg_frame_rate = (*codecContext)->framerate;

    // Open the output file if needed
    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Could not open output MP4 file.\n";
            return false;
        }
    }

    // Write header to the output file
    if (avformat_write_header(*formatContext, nullptr) < 0) {
        std::cerr << "Error: Could not write MP4 header.\n";
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Encode a frame: Convert the BGRA buffer (rgbBuffer) to YUV420P,
// set a presentation timestamp (PTS) based on real time, and send it to the encoder.
void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext,
                 uint8_t* rgbBuffer, int width, int height,
                 int64_t& frameCounter,
                 std::chrono::high_resolution_clock::time_point startTime) {
    static int frame_index = 0;
    
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

    // Convert the BGRA buffer to YUV420P using sws_scale
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
    frame_index++;
}

// -----------------------------------------------------------------------------
// The recordingThread handles starting the recording session:
// it initializes FFmpeg, the Desktop Duplication API, starts a capture thread to grab screen frames,
// and an encoding thread to encode and write frames.
void recordingThread(int width, int height) {
    // Generate output filename based on current time
    char outputFilename[128];
    time_t now = time(nullptr);
    strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mp4", localtime(&now));

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    if (!initializeFFmpeg(&formatContext, &codecContext, width, height, outputFilename)) {
        return;
    }

    // Initialize Desktop Duplication API for faster capture
    if (!initializeDesktopDuplication(width, height)) {
        std::cerr << "Error: Failed to initialize Desktop Duplication API.\n";
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
            // Allocate a new buffer for each frame (BGRA, 4 bytes per pixel)
            uint8_t* buffer = new uint8_t[width * height * 4];

            auto captureStart = std::chrono::high_resolution_clock::now();
            // Use the Desktop Duplication API to capture the screen
            if (!captureScreenDXGI(buffer, width, height)) {
                // If capture fails, wait briefly and try again
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                delete[] buffer;
                continue;
            }
            auto captureEnd = std::chrono::high_resolution_clock::now();
            int64_t captureDuration = std::chrono::duration_cast<std::chrono::microseconds>(captureEnd - captureStart).count();

            // Enforce a stable frame interval (targeting 60 FPS)
            int64_t waitTime = (1000000 / 60) - captureDuration;
            if (waitTime > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
            }

            auto currentTime = std::chrono::high_resolution_clock::now();
            int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();

            // Create a CapturedFrame and push it to the buffer
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
            // Encode the frame using the captured data
            encodeFrame(codecContext, formatContext, capFrame->data, width, height, localFrameCounter, startTime);
            delete[] capFrame->data;
            delete capFrame;
        }
    });

    captureThread.join();
    encodingThread.join();

    // Write trailer and clean up FFmpeg resources
    av_write_trailer(formatContext);
    avcodec_free_context(&codecContext);
    avformat_free_context(formatContext);

    // Release Desktop Duplication resources
    releaseDesktopDuplication();

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
