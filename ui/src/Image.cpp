// Image: loads a bitmap through WIC and draws it with the configured stretch, opacity and interpolation.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <utility>
#include <wincodec.h>

namespace msimeui
{
using namespace controls_detail;

namespace
{
SizeF ReadImageSize(const std::wstring &filePath)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (filePath.empty() ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
    {
        return {};
    }

    UINT width = 0;
    UINT height = 0;
    return SUCCEEDED(frame->GetSize(&width, &height)) ? SizeF{static_cast<float>(width), static_cast<float>(height)}
                                                      : SizeF{};
}
} // namespace

Image::Image(std::wstring filePath) : filePath_(std::move(filePath))
{
}

void Image::SetSource(std::wstring filePath)
{
    if (filePath_ == filePath)
    {
        return;
    }
    filePath_ = std::move(filePath);
    naturalSize_ = {};
    naturalSizeLoaded_ = false;
    InvalidateMeasure();
}

const std::wstring &Image::GetSource() const
{
    return filePath_;
}

void Image::SetStretch(ImageStretch stretch)
{
    if (stretch_ == stretch)
    {
        return;
    }
    stretch_ = stretch;
    InvalidateVisual();
}

void Image::SetOpacity(float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity_ == clamped)
    {
        return;
    }
    opacity_ = clamped;
    InvalidateVisual();
}

void Image::SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE interpolationMode)
{
    if (interpolationMode_ == interpolationMode)
    {
        return;
    }
    interpolationMode_ = interpolationMode;
    InvalidateVisual();
}

void Image::LoadNaturalSize()
{
    if (!naturalSizeLoaded_)
    {
        naturalSize_ = ReadImageSize(filePath_);
        naturalSizeLoaded_ = true;
    }
}

SizeF Image::Measure(const SizeF &availableSize)
{
    LoadNaturalSize();
    float width = naturalSize_.width;
    float height = naturalSize_.height;
    if (HasExplicitHeight() && !HasExplicitWidth() && naturalSize_.height > 0.0f)
    {
        width = naturalSize_.width * (explicitHeight_ / naturalSize_.height);
    }
    else if (HasExplicitWidth() && !HasExplicitHeight() && naturalSize_.width > 0.0f)
    {
        height = naturalSize_.height * (explicitWidth_ / naturalSize_.width);
    }
    return {std::min(width, availableSize.width), std::min(height, availableSize.height)};
}

void Image::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Image::Render(DeviceResources &deviceResources)
{
    D2D1_SIZE_F sourceSize = {};
    ID2D1Bitmap *bitmap = deviceResources.GetBitmapFromFile(filePath_, &sourceSize);
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!bitmap || !target || sourceSize.width <= 0.0f || sourceSize.height <= 0.0f || bounds_.width <= 0.0f ||
        bounds_.height <= 0.0f)
    {
        return;
    }

    RectF destination = bounds_;
    if (stretch_ == ImageStretch::None)
    {
        destination.width = std::min(sourceSize.width, bounds_.width);
        destination.height = std::min(sourceSize.height, bounds_.height);
    }
    else if (stretch_ == ImageStretch::Uniform || stretch_ == ImageStretch::UniformToFill)
    {
        const float scaleX = bounds_.width / sourceSize.width;
        const float scaleY = bounds_.height / sourceSize.height;
        const float scale = stretch_ == ImageStretch::Uniform ? std::min(scaleX, scaleY) : std::max(scaleX, scaleY);
        destination.width = sourceSize.width * scale;
        destination.height = sourceSize.height * scale;
        destination.x = bounds_.x + (bounds_.width - destination.width) * 0.5f;
        destination.y = bounds_.y + (bounds_.height - destination.height) * 0.5f;
    }

    const auto destinationRect = D2D1::RectF(destination.x, destination.y, destination.x + destination.width,
                                             destination.y + destination.height);
    if (stretch_ == ImageStretch::UniformToFill || stretch_ == ImageStretch::None)
    {
        target->PushAxisAlignedClip(
            D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    target->DrawBitmap(bitmap, destinationRect, opacity_, interpolationMode_);
    if (stretch_ == ImageStretch::UniformToFill || stretch_ == ImageStretch::None)
    {
        target->PopAxisAlignedClip();
    }
}
} // namespace msimeui
