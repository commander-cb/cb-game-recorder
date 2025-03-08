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

// We will use a local frame counter in the encoding thread

// -----------------------------------------------------------------------------
// Structure to hold captured frame data.
struct CapturedFrame {
    uint8_t* data;
    int64_t timestamp; // not used for encoding now
};

// -----------------------------------------------------------------------------
// FrameBuffer class for thread-safe storage.
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
            delete[] oldFrame->data;
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
// Global variables for Desktop Duplication API objects.
ID3D11Device*           g_d3dDevice      = nullptr;
ID3D11DeviceContext*    g_d3dContext     = nullptr;
IDXGIOutputDuplication* g_duplication    = nullptr;
ID3D11Texture2D*        g_stagingTexture = nullptr;

// -----------------------------------------------------------------------------
// Initialize Desktop Duplication using DirectX 11.
bool initializeDesktopDuplication(int width, int height) {
    HRESULT hr;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                           nullptr, 0, D3D11_SDK_VERSION, &g_d3dDevice, nullptr, &g_d3dContext);
    if (FAILED(hr)) {
        std::cerr << "Error: D3D11CreateDevice failed.\n";
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        std::cerr << "Error: QueryInterface for IDXGIDevice failed.\n";
        return false;
    }
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: GetParent for IDXGIAdapter failed.\n";
        return false;
    }
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: EnumOutputs failed.\n";
        return false;
    }
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: QueryInterface for IDXGIOutput1 failed.\n";
        return false;
    }
    hr = dxgiOutput1->DuplicateOutput(g_d3dDevice, &g_duplication);
    dxgiOutput1->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: DuplicateOutput failed. Ensure your system supports Desktop Duplication API.\n";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
// Release Desktop Duplication resources.
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
// Capture the screen using Desktop Duplication API.
bool captureScreenDXGI(uint8_t* buffer, int width, int height) {
    if (!g_duplication) {
        std::cerr << "Error: Desktop Duplication not initialized.\n";
        return false;
    }
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = g_duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
    if (FAILED(hr))
        return false;
    ID3D11Texture2D* acquiredDesktopImage = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&acquiredDesktopImage);
    desktopResource->Release();
    if (FAILED(hr)) {
        std::cerr << "Error: QueryInterface for acquired frame failed.\n";
        g_duplication->ReleaseFrame();
        return false;
    }
    g_d3dContext->CopyResource(g_stagingTexture, acquiredDesktopImage);
    acquiredDesktopImage->Release();
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_d3dContext->Map(g_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Error: Mapping staging texture failed.\n";
        g_duplication->ReleaseFrame();
        return false;
    }
    for (int y = 0; y < height; y++) {
        memcpy(buffer + y * width * 4, (uint8_t*)mapped.pData + y * mapped.RowPitch, width * 4);
    }
    g_d3dContext->Unmap(g_stagingTexture, 0);
    g_duplication->ReleaseFrame();
    return true;
}

// -----------------------------------------------------------------------------
// Initialize FFmpeg for Matroska output using libx264 encoder.
// Note: The time_base is now set to (1,60) to match 60 FPS.
bool initializeFFmpeg(AVFormatContext** formatContext, AVCodecContext** codecContext,
                      int width, int height, const char* filename) {
    avformat_alloc_output_context2(formatContext, nullptr, "matroska", filename);
    if (!*formatContext) {
        std::cerr << "Error: Could not allocate Matroska format context.\n";
        return false;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::cerr << "Error: libx264 encoder not found. Ensure FFmpeg is built with libx264 support.\n";
        return false;
    }
    *codecContext = avcodec_alloc_context3(codec);
    if (!*codecContext) {
        std::cerr << "Error: Could not allocate codec context.\n";
        return false;
    }
    (*codecContext)->width = width;
    (*codecContext)->height = height;
    // Set time base to (1,60) so that PTS values (in frame numbers) directly convert to seconds.
    (*codecContext)->time_base = (AVRational){1, 60};
    (*codecContext)->framerate = (AVRational){60, 1};
    (*codecContext)->pkt_timebase = (AVRational){1, 60};
    (*codecContext)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*codecContext)->gop_size = 120;   
    (*codecContext)->max_b_frames = 0;
    (*codecContext)->thread_count = 7;
    (*codecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; 
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "fast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "crf", "19", 0);//23def 
    if (avcodec_open2(*codecContext, codec, &opts) < 0) {
        std::cerr << "Error: Could not open libx264 codec.\n";
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);
    AVStream* stream = avformat_new_stream(*formatContext, nullptr);
    if (!stream) {
        std::cerr << "Error: Could not create stream.\n";
        return false;
    }
    avcodec_parameters_from_context(stream->codecpar, *codecContext);
    stream->time_base = (*codecContext)->time_base;
    stream->avg_frame_rate = (*codecContext)->framerate;
    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Could not open output file " << filename << ".\n";
            return false;
        }
    }
    if (avformat_write_header(*formatContext, nullptr) < 0) {
        std::cerr << "Error: Could not write header to output file.\n";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Encode a frame: Convert the BGRA buffer to YUV420P,
// and set the frame's PTS using a local encoding frame counter.
void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext,
                 uint8_t* rgbBuffer, int width, int height,
                 int64_t &frameCounter,
                 std::chrono::high_resolution_clock::time_point startTime)
{
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
    
    // Compute elapsed wall-clock time since startTime (in microseconds).
    auto now = std::chrono::high_resolution_clock::now();
    int64_t elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
    
    // Divide the elapsed time by a constant multiplier to "cheat" the system.
    // For example, if multiplier is 20.0, the effective duration is elapsed/20.
    const double multiplier = 1000.0; // Adjust this value as needed./20//////1000
    frame->pts = static_cast<int64_t>(elapsed / multiplier);
    
    // Convert the BGRA buffer to YUV420P using sws_scale.
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
    
    // Send the frame to the encoder.
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
// The recordingThread initializes FFmpeg, Desktop Duplication,
// and launches capture and encoding threads.
void recordingThread(int width, int height) {
    char outputFilename[128];
    time_t now = time(nullptr);
    strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mkv", localtime(&now));
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    if (!initializeFFmpeg(&formatContext, &codecContext, width, height, outputFilename))
        return;
    if (!initializeDesktopDuplication(width, height)) {
        std::cerr << "Error: Failed to initialize Desktop Duplication API.\n";
        return;
    }
    FrameBuffer frameBuffer(60);
    // Local encoding frame counter starts at 0.
    int64_t localFrameCounter = 0;
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::thread captureThread([&]() {
        std::vector<uint8_t> lastValidFrame(width * height * 4, 0);
        while (isRecording) {
            uint8_t* buffer = new uint8_t[width * height * 4];
            auto captureStart = std::chrono::high_resolution_clock::now();
            if (!captureScreenDXGI(buffer, width, height))
                memcpy(buffer, lastValidFrame.data(), width * height * 4);
            else
                memcpy(lastValidFrame.data(), buffer, width * height * 4);
            auto captureEnd = std::chrono::high_resolution_clock::now();
            int64_t captureDuration = std::chrono::duration_cast<std::chrono::microseconds>(captureEnd - captureStart).count();
            int64_t waitTime = (1000000 / 60) - captureDuration;
            if (waitTime > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
            
            CapturedFrame* capFrame = new CapturedFrame;
            capFrame->data = buffer;
            capFrame->timestamp = 0; // not used for encoding
            frameBuffer.push(capFrame);
        }
    });
    
    std::thread encodingThread([&]() {
        while (isRecording || !frameBuffer.isEmpty()) {
            CapturedFrame* capFrame = frameBuffer.pop();
            if (!capFrame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            encodeFrame(codecContext, formatContext, capFrame->data, width, height, localFrameCounter, startTime);
            delete[] capFrame->data;
            delete capFrame;
        }
    });
    
    captureThread.join();
    encodingThread.join();
    
    // Flush the encoder
    {
        AVPacket* packet = av_packet_alloc();
        avcodec_send_frame(codecContext, nullptr);
        while (avcodec_receive_packet(codecContext, packet) >= 0) {
            av_interleaved_write_frame(formatContext, packet);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    
    av_write_trailer(formatContext);
    avcodec_free_context(&codecContext);
    avformat_free_context(formatContext);
    releaseDesktopDuplication();
    
    std::cout << "Recording saved to " << outputFilename << ".\n";
}

// -----------------------------------------------------------------------------
// Low-level keyboard hook to toggle recording with the 'U' key.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;
        if (kbStruct->vkCode == 0x55 && wParam == WM_KEYDOWN) {
            isRecording = !isRecording;
            cv.notify_all();
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// -----------------------------------------------------------------------------
// Main function: Sets up the recording thread and keyboard hook.
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
