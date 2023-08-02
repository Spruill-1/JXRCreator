#include "pch.h"

using namespace winrt;
using namespace Windows::Foundation;

#define RETURN_FAILURE(func)                    \
hr = func;                                      \
if (FAILED(hr))                                 \
{                                               \
std::cerr << "Error code: " << hr << std::endl; \
return hr;                                      \
}                                               \

const size_t numThreads = 10;
const float maxChannelValue = 5.0f;

struct PixelStruct
{
    float red;
    float green;
    float blue;
    float padding;
};

int main()
{
    HRESULT hr = S_OK;
    init_apartment();

    winrt::com_ptr<IWICImagingFactory> factory = nullptr;
    winrt::com_ptr<IWICBitmap> bitmap = nullptr;

    INT width = 17000;
    INT height = 480;
    WICPixelFormatGUID formatGUID = GUID_WICPixelFormat128bppRGBFloat;

    WICRect rcLock = { 0, 0, width, height };

    RETURN_FAILURE(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)
    ));

    RETURN_FAILURE(factory->CreateBitmap(width, height, formatGUID, WICBitmapCacheOnDemand, bitmap.put()));

    // Lock the bitmap in a scoped block to properly handle releasing the lock.
    {
        winrt::com_ptr<IWICBitmapLock> lock = nullptr;
        RETURN_FAILURE(bitmap->Lock(&rcLock, WICBitmapLockWrite, lock.put()));

        UINT bufferSize = 0;
        UINT bufferStride = 0;
        BYTE* data = nullptr;

        // Retrieve the stride.
        RETURN_FAILURE(lock->GetStride(&bufferStride));

        RETURN_FAILURE(lock->GetDataPointer(&bufferSize, &data));

        // Each row starts at *previousRow + bufferStride. Spin up a few threads to help speed this up
        std::vector<std::thread> threads(numThreads);
        for (auto i = 0; i < numThreads; ++i)
        {
            threads[i] = std::thread([&, i]() 
            {
                for (auto y = i * height / numThreads; y < (i + 1) * height / numThreads; ++y)
                {
                    BYTE* row = data + y * bufferStride;
                    for (auto x = 0; x < width; ++x)
                    {
                        PixelStruct* pixel = reinterpret_cast<PixelStruct*>((BYTE*)(row + x * sizeof(PixelStruct)));
                        pixel->red = (maxChannelValue * ((float)(x + y) / (float)(width + height)));
                    }
                }
            });
        }

        for (auto& thread : threads)
        {
            if (thread.joinable()) thread.join();
        }
    }

    // Save the data to an image file
    {
        // Create the wic stream for the file
        winrt::com_ptr<IWICStream> wicStream = nullptr;
        RETURN_FAILURE(factory->CreateStream(wicStream.put()));
        RETURN_FAILURE(wicStream->InitializeFromFilename(L"output.jxr", GENERIC_WRITE));

        // Create the wic encoder
        winrt::com_ptr<IWICBitmapEncoder> wicEncoder = nullptr;
        RETURN_FAILURE(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, wicEncoder.put()));
        RETURN_FAILURE(wicEncoder->Initialize(wicStream.get(), WICBitmapEncoderNoCache));

        // Create and initialize the wic frame
        winrt::com_ptr<IWICBitmapFrameEncode> frame = nullptr;
        RETURN_FAILURE(wicEncoder->CreateNewFrame(frame.put(), nullptr));
        RETURN_FAILURE(frame->Initialize(nullptr));
        RETURN_FAILURE(frame->SetSize(width, height));

        RETURN_FAILURE(frame->SetPixelFormat(&formatGUID));
        RETURN_FAILURE(frame->WriteSource(bitmap.get(), nullptr));
        RETURN_FAILURE(frame->Commit());
        RETURN_FAILURE(wicEncoder->Commit());
    }

    return hr;
}
