//
//
//
//
//
//
//
//
//




#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <chrono>  // For timestamp generation

#pragma comment(lib, "ole32.lib")

DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM,
    0x00000001, 0x0000, 0x0010,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
    0x00000003, 0x0000, 0x0010,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#define EXIT_ON_ERROR(hres, message)  \
    if (FAILED(hres)) { std::cerr << "Error: " << message << " (Line " << __LINE__ << ", HRESULT: 0x" << std::hex << hres << ")" << std::endl; return -1; }

#define SAFE_RELEASE(p)  \
    if ((p)) { (p)->Release(); (p) = NULL; }

// Function to generate a timestamped filename
std::string generateTimestampedFilename() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d_%H-%M-%S");
    
    // Return the filename with timestamp
    return "output_" + ss.str() + ".wav";
}

bool ConvertToPcmFormat(WAVEFORMATEX *pwfx) {
    if (pwfx->wFormatTag == WAVE_FORMAT_PCM) {
        if (pwfx->wBitsPerSample > 16) {
            std::cout << "Reducing bits per sample from " << pwfx->wBitsPerSample << " to 16.\n";
            pwfx->wBitsPerSample = 16;
            pwfx->nBlockAlign = pwfx->nChannels * (pwfx->wBitsPerSample / 8);
            pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
        }
        return true;
    }

    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto pExtensible = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(pwfx);
        if (pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            std::cout << "Converting IEEE_FLOAT to PCM.\n";
            pwfx->wFormatTag = WAVE_FORMAT_PCM;
            pwfx->wBitsPerSample = 16;  // Convert to 16 bits
            pwfx->nBlockAlign = pwfx->nChannels * (pwfx->wBitsPerSample / 8);
            pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
            pwfx->cbSize = 0;  // No extra data for PCM
            return true;
        } else if (pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            std::cout << "Already PCM format.\n";
            return true;
        }
    }

    std::cerr << "Unsupported format tag or SubFormat.\n";
    return false;
}

void WriteWavHeader(std::ofstream &file, int sampleRate, int channels, int bitsPerSample, int dataSize) {
    file.write("RIFF", 4);
    int chunkSize = 36 + dataSize;  // RIFF chunk size
    file.write(reinterpret_cast<const char *>(&chunkSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);

    int subChunk1Size = 16;  // PCM size
    file.write(reinterpret_cast<const char *>(&subChunk1Size), 4);
    short audioFormat = 1;  // PCM format
    file.write(reinterpret_cast<const char *>(&audioFormat), 2);
    file.write(reinterpret_cast<const char *>(&channels), 2);
    file.write(reinterpret_cast<const char *>(&sampleRate), 4);
    int byteRate = sampleRate * channels * (bitsPerSample / 8);
    file.write(reinterpret_cast<const char *>(&byteRate), 4);
    short blockAlign = channels * (bitsPerSample / 8);
    file.write(reinterpret_cast<const char *>(&blockAlign), 2);
    file.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

    file.write("data", 4);
    file.write(reinterpret_cast<const char *>(&dataSize), 4);
}

void FinalizeWavHeader(std::ofstream &file, int totalDataBytes) {
    file.seekp(4, std::ios::beg);
    int chunkSize = 36 + totalDataBytes;
    file.write(reinterpret_cast<const char *>(&chunkSize), 4);

    file.seekp(40, std::ios::beg);
    file.write(reinterpret_cast<const char *>(&totalDataBytes), 4);
}

int main() {
    HRESULT hr;
    IMMDeviceEnumerator *pEnumerator = NULL;
    IMMDevice *pDevice = NULL;
    IAudioClient *pAudioClient = NULL;
    IAudioCaptureClient *pCaptureClient = NULL;
    WAVEFORMATEX *pwfx = NULL;
    std::ofstream outFile;

    hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM." << std::endl;
        return -1;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr)) {
        std::cerr << "Failed to create device enumerator." << std::endl;
        CoUninitialize();
        return -1;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        std::cerr << "Failed to get default audio endpoint." << std::endl;
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&pAudioClient);
    if (FAILED(hr)) {
        std::cerr << "Failed to activate audio client." << std::endl;
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        std::cerr << "Failed to get mix format." << std::endl;
        SAFE_RELEASE(pAudioClient);
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    if (!ConvertToPcmFormat(pwfx)) {
        std::cerr << "Unsupported audio format." << std::endl;
        SAFE_RELEASE(pAudioClient);
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    REFERENCE_TIME hnsBufferDuration = 10000000;  // 1 second
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsBufferDuration,
        0,
        pwfx,
        NULL
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize audio client." << std::endl;
        SAFE_RELEASE(pAudioClient);
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    hr = pAudioClient->GetService(IID_PPV_ARGS(&pCaptureClient));
    if (FAILED(hr)) {
        std::cerr << "Failed to get audio capture client." << std::endl;
        SAFE_RELEASE(pAudioClient);
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    // Generate timestamped filename
    std::string filename = generateTimestampedFilename();
    outFile.open(filename, std::ios::binary);
    if (!outFile.is_open()) {
        std::cerr << "Failed to open output file." << std::endl;
        SAFE_RELEASE(pCaptureClient);
        SAFE_RELEASE(pAudioClient);
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    WriteWavHeader(outFile, pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample, 0);

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "Failed to start audio capture." << std::endl;
        outFile.close();
        SAFE_RELEASE(pCaptureClient);
        SAFE_RELEASE(pAudioClient);
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pEnumerator);
        CoUninitialize();
        return -1;
    }

    BYTE *pData = nullptr;
    DWORD flags;
    UINT32 packetLength = 0;
    int totalDataWritten = 0;

    // Timer to stop after 30 seconds
    auto startTime = std::chrono::high_resolution_clock::now();

    while (true) {
        auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 3000) {
            break;
        }

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength != 0) {
            UINT32 numFramesAvailable;
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            int bytesToWrite = numFramesAvailable * pwfx->nBlockAlign;
            if (pwfx->wBitsPerSample == 32) {
                // Downsample 32-bit audio to 16-bit
                auto input = reinterpret_cast<int32_t *>(pData);
                int16_t *output = new int16_t[numFramesAvailable * pwfx->nChannels];
                for (UINT32 i = 0; i < numFramesAvailable * pwfx->nChannels; ++i) {
                    output[i] = input[i] >> 16;  // Shift 32-bit to 16-bit
                }
                outFile.write(reinterpret_cast<const char *>(output), numFramesAvailable * pwfx->nChannels * sizeof(int16_t));
                delete[] output;
            } else {
                outFile.write(reinterpret_cast<const char *>(pData), bytesToWrite);
            }
            totalDataWritten += bytesToWrite;

            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) break;

            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }

    pAudioClient->Stop();
    FinalizeWavHeader(outFile, totalDataWritten);
    outFile.close();

    SAFE_RELEASE(pCaptureClient);
    SAFE_RELEASE(pAudioClient);
    SAFE_RELEASE(pDevice);
    SAFE_RELEASE(pEnumerator);
    CoUninitialize();

    return 0;
}
