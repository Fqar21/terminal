// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "BackendD3D.h"

#include <til/unicode.h>

#include <custom_shader_ps.h>
#include <custom_shader_vs.h>
#include <shader_cs.h>

#include "BuiltinGlyphs.h"
#include "dwrite.h"
#include "wic.h"
#include "../../types/inc/ColorFix.hpp"
#include "../../types/inc/convert.hpp"

#if ATLAS_DEBUG_SHOW_DIRTY || ATLAS_DEBUG_COLORIZE_GLYPH_ATLAS
#include <til/colorbrewer.h>
#endif

TIL_FAST_MATH_BEGIN

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 26440) // Function '...' can be declared 'noexcept'(f.6).
// This code packs various data into smaller-than-int types to save both CPU and GPU memory. This warning would force
// us to add dozens upon dozens of gsl::narrow_cast<>s throughout the file which is more annoying than helpful.
#pragma warning(disable : 4242) // '=': conversion from '...' to '...', possible loss of data
#pragma warning(disable : 4244) // 'initializing': conversion from '...' to '...', possible loss of data
#pragma warning(disable : 4267) // 'argument': conversion from '...' to '...', possible loss of data
#pragma warning(disable : 4838) // conversion from '...' to '...' requires a narrowing conversion
#pragma warning(disable : 26472) // Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).
#pragma warning(disable : 26490) // Don't use reinterpret_cast (type.1).

// Initializing large arrays can be very costly compared to how cheap some of these functions are.
#define ALLOW_UNINITIALIZED_BEGIN _Pragma("warning(push)") _Pragma("warning(disable : 26494)")
#define ALLOW_UNINITIALIZED_END _Pragma("warning(pop)")

using namespace Microsoft::Console::Render::Atlas;

static constexpr D2D1_MATRIX_3X2_F identityTransform{ .m11 = 1, .m22 = 1 };
static constexpr D2D1_COLOR_F whiteColor{ 1, 1, 1, 1 };

static u64 queryPerfFreq() noexcept
{
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return std::bit_cast<u64>(li.QuadPart);
}

static u64 queryPerfCount() noexcept
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return std::bit_cast<u64>(li.QuadPart);
}

BackendD3D::BackendD3D(const RenderingPayload& p)
{
    THROW_IF_FAILED(p.device->CreateComputeShader(&shader_cs[0], sizeof(shader_cs), nullptr, _computeShader.addressof()));

    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(ConstBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(p.device->CreateBuffer(&desc, nullptr, _constantBuffer.addressof()));
    }

#if ATLAS_DEBUG_SHADER_HOT_RELOAD
    _sourceDirectory = std::filesystem::path{ __FILE__ }.parent_path();
    _sourceCodeWatcher = wil::make_folder_change_reader_nothrow(_sourceDirectory.c_str(), false, wil::FolderChangeEvents::FileName | wil::FolderChangeEvents::LastWriteTime, [this](wil::FolderChangeEvent, PCWSTR path) {
        if (til::ends_with(path, L".hlsl"))
        {
            auto expected = INT64_MAX;
            const auto invalidationTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            _sourceCodeInvalidationTime.compare_exchange_strong(expected, invalidationTime.time_since_epoch().count(), std::memory_order_relaxed);
        }
    });
#endif
}

#pragma warning(suppress : 26432) // If you define or delete any default operation in the type '...', define or delete them all (c.21).
BackendD3D::~BackendD3D()
{
    // In case an exception is thrown for some reason between BeginDraw() and EndDraw()
    // we still technically need to call EndDraw() before releasing any resources.
    if (_d2dBeganDrawing)
    {
#pragma warning(suppress : 26447) // The function is declared 'noexcept' but calls function '...' which may throw exceptions (f.6).
        LOG_IF_FAILED(_d2dRenderTarget->EndDraw());
    }
}

void BackendD3D::ReleaseResources() noexcept
{
    _renderTargetView.reset();
    _customRenderTargetView.reset();
    // Ensure _handleSettingsUpdate() is called so that _renderTarget gets recreated.
    _generation = {};
}

void BackendD3D::Render(RenderingPayload& p)
{
    if (_generation != p.s.generation())
    {
        _handleSettingsUpdate(p);
    }

    _debugUpdateShaders(p);

    // Invalidating the render target helps with spotting invalid quad instances and Present1() bugs.
#if ATLAS_DEBUG_SHOW_DIRTY || ATLAS_DEBUG_DUMP_RENDER_TARGET
    {
        static constexpr f32 clearColor[4]{};
        p.deviceContext->ClearView(_renderTargetView.get(), &clearColor[0], nullptr, 0);
    }
#endif

    _drawBackground(p);
    _drawCursorBackground(p);
    _drawText(p);
    _drawSelection(p);
    _debugShowDirty(p);
    _flushQuads(p);

    if (_customPixelShader)
    {
        _executeCustomShader(p);
    }

    _debugDumpRenderTarget(p);
}

bool BackendD3D::RequiresContinuousRedraw() noexcept
{
    return _requiresContinuousRedraw;
}

void BackendD3D::_handleSettingsUpdate(const RenderingPayload& p)
{
    if (!_renderTargetView)
    {
        wil::com_ptr<ID3D11Texture2D> buffer;
        THROW_IF_FAILED(p.swapChain.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(buffer.addressof())));
        THROW_IF_FAILED(p.device->CreateUnorderedAccessView(buffer.get(), nullptr, _renderTargetView.put()));
    }

    const auto fontChanged = _fontGeneration != p.s->font.generation();
    const auto miscChanged = _miscGeneration != p.s->misc.generation();
    const auto cellCountChanged = _viewportCellCount != p.s->viewportCellCount;

    if (fontChanged)
    {
        _updateFontDependents(p);
    }
    if (miscChanged)
    {
        _recreateCustomShader(p);
    }
    if (cellCountChanged)
    {
        _recreateBackgroundColorBitmap(p);
    }

    // Similar to _renderTargetView above, we might have to recreate the _customRenderTargetView whenever _swapChainManager
    // resets it. We only do it after calling _recreateCustomShader however, since that sets the _customPixelShader.
    if (_customPixelShader && !_customRenderTargetView)
    {
        _recreateCustomRenderTargetView(p);
    }

    _recreateConstBuffer(p);
    _setupDeviceContextState(p);

    _generation = p.s.generation();
    _fontGeneration = p.s->font.generation();
    _miscGeneration = p.s->misc.generation();
    _targetSize = p.s->targetSize;
    _viewportCellCount = p.s->viewportCellCount;
}

void BackendD3D::_updateFontDependents(const RenderingPayload& p)
{
    const auto& font = *p.s->font;

    // Curlyline is drawn with a desired height relative to the font size. The
    // baseline of curlyline is at the middle of singly underline. When there's
    // limited space to draw a curlyline, we apply a limit on the peak height.
    {
        const int cellHeight = font.cellSize.y;
        const int duTop = font.doubleUnderline[0].position;
        const int duBottom = font.doubleUnderline[1].position;
        const int duHeight = font.doubleUnderline[0].height;

        // This gives it the same position and height as our double-underline. There's no particular reason for that, apart from
        // it being simple to implement and robust against more peculiar fonts with unusually large/small descenders, etc.
        // We still need to ensure though that it doesn't clip out of the cellHeight at the bottom, which is why `position` has a min().
        const auto height = std::max(3, duBottom + duHeight - duTop);
        const auto position = std::min(duTop, cellHeight - height);

        _curlyLineHalfHeight = height * 0.5f;
        _curlyUnderline.position = gsl::narrow_cast<u16>(position);
        _curlyUnderline.height = gsl::narrow_cast<u16>(height);
    }

    DWrite_GetRenderParams(p.dwriteFactory.get(), &_gamma, &_cleartypeEnhancedContrast, &_grayscaleEnhancedContrast, _textRenderingParams.put());
    // Clearing the atlas requires BeginDraw(), which is expensive. Defer this until we need Direct2D anyways.
    _fontChangedResetGlyphAtlas = true;
    _textShadingType = font.antialiasingMode == AntialiasingMode::ClearType ? ShadingType::TextClearType : ShadingType::TextGrayscale;

    // _ligatureOverhangTriggerLeft/Right are essentially thresholds for a glyph's width at
    // which point we consider it wider than allowed and "this looks like a coding ligature".
    // See _drawTextOverlapSplit for more information about what this does.
    {
        // No ligatures -> No thresholds.
        auto ligaturesDisabled = false;
        for (const auto& feature : font.fontFeatures)
        {
            if (feature.nameTag == DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES)
            {
                ligaturesDisabled = !feature.parameter;
                break;
            }
        }

        if (ligaturesDisabled)
        {
            _ligatureOverhangTriggerLeft = til::CoordTypeMin;
            _ligatureOverhangTriggerRight = til::CoordTypeMax;
        }
        else
        {
            const auto halfCellWidth = font.cellSize.x / 2;
            _ligatureOverhangTriggerLeft = -halfCellWidth;
            _ligatureOverhangTriggerRight = font.advanceWidth + halfCellWidth;
        }
    }

    if (_d2dRenderTarget)
    {
        _d2dRenderTargetUpdateFontSettings(p);
    }

    _softFontBitmap.reset();
}

void BackendD3D::_d2dRenderTargetUpdateFontSettings(const RenderingPayload& p) const noexcept
{
    const auto& font = *p.s->font;
    _d2dRenderTarget->SetDpi(font.dpi, font.dpi);
    _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(font.antialiasingMode));
}

void BackendD3D::_recreateCustomShader(const RenderingPayload& p)
{
    _customRenderTargetView.reset();
    _customOffscreenTexture.reset();
    _customOffscreenTextureView.reset();
    _customVertexShader.reset();
    _customPixelShader.reset();
    _customShaderConstantBuffer.reset();
    _customShaderSamplerState.reset();
    _customShaderTexture.reset();
    _customShaderTextureView.reset();
    _requiresContinuousRedraw = false;

    if (!p.s->misc->customPixelShaderPath.empty())
    {
        const char* target = nullptr;
        switch (p.device->GetFeatureLevel())
        {
        case D3D_FEATURE_LEVEL_10_0:
            target = "ps_4_0";
            break;
        case D3D_FEATURE_LEVEL_10_1:
            target = "ps_4_1";
            break;
        default:
            target = "ps_5_0";
            break;
        }

        static constexpr auto flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR
#ifdef NDEBUG
            | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
            // Only enable strictness and warnings in DEBUG mode
            //  as these settings makes it very difficult to develop
            //  shaders as windows terminal is not telling the user
            //  what's wrong, windows terminal just fails.
            //  Keep it in DEBUG mode to catch errors in shaders
            //  shipped with windows terminal
            | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ p.s->misc->customPixelShaderPath.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ flags,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        if (SUCCEEDED(hr))
        {
            THROW_IF_FAILED(p.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _customPixelShader.addressof()));

            // Try to determine whether the shader uses the Time variable
            wil::com_ptr<ID3D11ShaderReflection> reflector;
            if (SUCCEEDED_LOG(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(reflector.addressof()))))
            {
                // Depending on the version of the d3dcompiler_*.dll, the next two functions either return nullptr
                // on failure or an instance of CInvalidSRConstantBuffer or CInvalidSRVariable respectively,
                // which cause GetDesc() to return E_FAIL. In other words, we have to assume that any failure in the
                // next few lines indicates that the cbuffer is entirely unused (--> _requiresContinuousRedraw=false).
                if (ID3D11ShaderReflectionConstantBuffer* constantBufferReflector = reflector->GetConstantBufferByIndex(0)) // shader buffer
                {
                    if (ID3D11ShaderReflectionVariable* variableReflector = constantBufferReflector->GetVariableByIndex(0)) // time
                    {
                        D3D11_SHADER_VARIABLE_DESC variableDescriptor;
                        if (SUCCEEDED(variableReflector->GetDesc(&variableDescriptor)))
                        {
                            // only if time is used
                            _requiresContinuousRedraw = WI_IsFlagSet(variableDescriptor.uFlags, D3D_SVF_USED);
                        }
                    }
                }
            }
            else
            {
                // Unless we can determine otherwise, assume this shader requires evaluation every frame
                _requiresContinuousRedraw = true;
            }
        }
        else
        {
            if (error)
            {
                if (p.warningCallback)
                {
                    //to handle compile time errors
                    const std::string_view errMsgStrView{ static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize() };
                    const auto errMsgWstring = ConvertToW(CP_ACP, errMsgStrView);
                    p.warningCallback(D2DERR_SHADER_COMPILE_FAILED, errMsgWstring);
                }
            }
            else
            {
                if (p.warningCallback)
                {
                    //to handle errors such as file not found, path not found, access denied
                    p.warningCallback(hr, p.s->misc->customPixelShaderPath);
                }
            }
        }

        if (!p.s->misc->customPixelShaderImagePath.empty())
        {
            try
            {
                WIC::LoadTextureFromFile(p.device.get(), p.s->misc->customPixelShaderImagePath.c_str(), _customShaderTexture.addressof(), _customShaderTextureView.addressof());
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                _customPixelShader.reset();
                if (p.warningCallback)
                {
                    p.warningCallback(D2DERR_SHADER_COMPILE_FAILED, p.s->misc->customPixelShaderImagePath);
                }
            }
        }
    }
    else if (p.s->misc->useRetroTerminalEffect)
    {
        THROW_IF_FAILED(p.device->CreatePixelShader(&custom_shader_ps[0], sizeof(custom_shader_ps), nullptr, _customPixelShader.put()));
    }

    if (_customPixelShader)
    {
        THROW_IF_FAILED(p.device->CreateVertexShader(&custom_shader_vs[0], sizeof(custom_shader_vs), nullptr, _customVertexShader.put()));

        {
            static constexpr D3D11_BUFFER_DESC desc{
                .ByteWidth = sizeof(CustomConstBuffer),
                .Usage = D3D11_USAGE_DYNAMIC,
                .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
                .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
            };
            THROW_IF_FAILED(p.device->CreateBuffer(&desc, nullptr, _customShaderConstantBuffer.put()));
        }

        {
            static constexpr D3D11_SAMPLER_DESC desc{
                .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                .AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
                .AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
                .AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
                .MaxAnisotropy = 1,
                .ComparisonFunc = D3D11_COMPARISON_ALWAYS,
                .MaxLOD = D3D11_FLOAT32_MAX,
            };
            THROW_IF_FAILED(p.device->CreateSamplerState(&desc, _customShaderSamplerState.put()));
        }

        // Since floats are imprecise we need to constrain the time value into a range that can be accurately represented.
        // Assuming a monitor refresh rate of 1000 Hz, we can still easily represent 1000 seconds accurately (roughly 16 minutes).
        // 10000 seconds would already result in a 50% error. So to avoid this, we use queryPerfCount() modulo _customShaderPerfTickMod.
        // The use of a power of 10 is intentional, because shaders are often periodic and this makes any decimal multiplier up to 3 fractional
        // digits not break the periodicity. For instance, with a wraparound of 1000 seconds sin(1.234*x) is still perfectly periodic.
        const auto freq = queryPerfFreq();
        _customShaderPerfTickMod = freq * 1000;
        _customShaderSecsPerPerfTick = 1.0f / freq;
    }
}

void BackendD3D::_recreateCustomRenderTargetView(const RenderingPayload& p)
{
    // Avoid memory usage spikes by releasing memory first.
    _customOffscreenTexture.reset();
    _customOffscreenTextureView.reset();

    const D3D11_TEXTURE2D_DESC desc{
        .Width = p.s->targetSize.x,
        .Height = p.s->targetSize.y,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
    };
    THROW_IF_FAILED(p.device->CreateTexture2D(&desc, nullptr, _customOffscreenTexture.addressof()));
    THROW_IF_FAILED(p.device->CreateShaderResourceView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureView.addressof()));
    THROW_IF_FAILED(p.device->CreateRenderTargetView(_customOffscreenTexture.get(), nullptr, _customRenderTargetView.addressof()));
}

void BackendD3D::_recreateBackgroundColorBitmap(const RenderingPayload& p)
{
    // Avoid memory usage spikes by releasing memory first.
    _backgroundBitmap.reset();
    _backgroundBitmapView.reset();

    const D3D11_TEXTURE2D_DESC desc{
        .Width = p.s->viewportCellCount.x,
        .Height = p.s->viewportCellCount.y,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    THROW_IF_FAILED(p.device->CreateTexture2D(&desc, nullptr, _backgroundBitmap.addressof()));
    THROW_IF_FAILED(p.device->CreateShaderResourceView(_backgroundBitmap.get(), nullptr, _backgroundBitmapView.addressof()));
    _backgroundBitmapGeneration = {};
}

void BackendD3D::_recreateConstBuffer(const RenderingPayload& p) const
{
    {
        ConstBuffer data{};
        data.backgroundColor = colorFromU32Premultiply<f32x4>(p.s->misc->backgroundColor);
        data.backgroundCellSize = { static_cast<f32>(p.s->font->cellSize.x), static_cast<f32>(p.s->font->cellSize.y) };
        data.backgroundCellCount = { static_cast<f32>(p.s->viewportCellCount.x), static_cast<f32>(p.s->viewportCellCount.y) };
        DWrite_GetGammaRatios(_gamma, data.gammaRatios);
        data.enhancedContrast = p.s->font->antialiasingMode == AntialiasingMode::ClearType ? _cleartypeEnhancedContrast : _grayscaleEnhancedContrast;
        data.underlineWidth = p.s->font->underline.height;
        data.doubleUnderlineWidth = p.s->font->doubleUnderline[0].height;
        data.curlyLineHalfHeight = _curlyLineHalfHeight;
        data.shadedGlyphDotSize = std::max(1.0f, std::roundf(std::max(p.s->font->cellSize.x / 16.0f, p.s->font->cellSize.y / 32.0f)));
        p.deviceContext->UpdateSubresource(_constantBuffer.get(), 0, nullptr, &data, 0, 0);
    }
}

void BackendD3D::_setupDeviceContextState(const RenderingPayload& p)
{
}

void BackendD3D::_debugUpdateShaders(const RenderingPayload& p) noexcept
{
#if ATLAS_DEBUG_SHADER_HOT_RELOAD
    try
    {
        const auto invalidationTime = _sourceCodeInvalidationTime.load(std::memory_order_relaxed);

        if (invalidationTime == INT64_MAX || invalidationTime > std::chrono::steady_clock::now().time_since_epoch().count())
        {
            return;
        }

        _sourceCodeInvalidationTime.store(INT64_MAX, std::memory_order_relaxed);

        static constexpr auto flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS
#ifndef NDEBUG
            | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION
#endif
            ;

        static const auto compile = [](const std::filesystem::path& path, const char* target) {
            wil::com_ptr<ID3DBlob> error;
            wil::com_ptr<ID3DBlob> blob;
            const auto hr = D3DCompileFromFile(
                /* pFileName   */ path.c_str(),
                /* pDefines    */ nullptr,
                /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
                /* pEntrypoint */ "main",
                /* pTarget     */ target,
                /* Flags1      */ flags,
                /* Flags2      */ 0,
                /* ppCode      */ blob.addressof(),
                /* ppErrorMsgs */ error.addressof());

            if (error)
            {
                std::thread t{ [error = std::move(error)]() noexcept {
                    MessageBoxA(nullptr, static_cast<const char*>(error->GetBufferPointer()), "Compilation error", MB_ICONERROR | MB_OK);
                } };
                t.detach();
            }

            THROW_IF_FAILED(hr);
            return blob;
        };

        const auto blob = compile(_sourceDirectory / L"shader_cs.hlsl", "vs_4_0");
        wil::com_ptr<ID3D11ComputeShader> computeShader;
        THROW_IF_FAILED(p.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, computeShader.addressof()));
        _computeShader = std::move(computeShader);

        _setupDeviceContextState(p);
    }
    CATCH_LOG()
#endif
}

void BackendD3D::_d2dBeginDrawing() noexcept
{
    if (!_d2dBeganDrawing)
    {
        _d2dRenderTarget->BeginDraw();
        _d2dBeganDrawing = true;
    }
}

void BackendD3D::_d2dEndDrawing()
{
    if (_d2dBeganDrawing)
    {
        THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
        _d2dBeganDrawing = false;
    }
}

void BackendD3D::_resetGlyphAtlas(const RenderingPayload& p)
{
    // The index returned by _BitScanReverse is undefined when the input is 0. We can simultaneously guard
    // against that and avoid unreasonably small textures, by clamping the min. texture size to `minArea`.
    // `minArea` results in a 64kB RGBA texture which is the min. alignment for placed memory.
    static constexpr u32 minArea = 1024 * 1024;
    static constexpr u32 maxArea = D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION * D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

    const auto cellArea = static_cast<u32>(p.s->font->cellSize.x) * p.s->font->cellSize.y;
    const auto targetArea = static_cast<u32>(p.s->targetSize.x) * p.s->targetSize.y;

    const auto minAreaByFont = cellArea * 95; // Covers all printable ASCII characters
    const auto minAreaByGrowth = static_cast<u32>(_rectPacker.width) * _rectPacker.height * 2;

    // It's hard to say what the max. size of the cache should be. Optimally I think we should use as much
    // memory as is available, but the rendering code in this project is a big mess and so integrating
    // memory pressure feedback (RegisterVideoMemoryBudgetChangeNotificationEvent) is rather difficult.
    // As an alternative I'm using 1.25x the size of the swap chain. The 1.25x is there to avoid situations, where
    // we're locked into a state, where on every render pass we're starting with a half full atlas, drawing once,
    // filling it with the remaining half and drawing again, requiring two rendering passes on each frame.
    const auto maxAreaByFont = targetArea + targetArea / 4;

    auto area = std::min(maxAreaByFont, std::max(minAreaByFont, minAreaByGrowth));
    area = clamp(area, minArea, maxArea);

    // This block of code calculates the size of a power-of-2 texture that has an area larger than the given `area`.
    // For instance, for an area of 985x1946 = 1916810 it would result in a u/v of 2048x1024 (area = 2097152).
    // This has 2 benefits: GPUs like power-of-2 textures and it ensures that we don't resize the texture
    // every time you resize the window by a pixel. Instead it only grows/shrinks by a factor of 2.
    unsigned long index;
    _BitScanReverse(&index, area - 1);
    const auto u = static_cast<u16>(1u << ((index + 2) / 2));
    const auto v = static_cast<u16>(1u << ((index + 1) / 2));

    if (u != _rectPacker.width || v != _rectPacker.height)
    {
        _resizeGlyphAtlas(p, u, v);
    }

    stbrp_init_target(&_rectPacker, u, v, _rectPackerData.data(), _rectPackerData.size());

    // This is a little imperfect, because it only releases the memory of the glyph mappings, not the memory held by
    // any DirectWrite fonts. On the other side, the amount of fonts on a system is always finite, where "finite"
    // is pretty low, relatively speaking. Additionally this allows us to cache the boxGlyphs map indefinitely.
    // It's not great, but it's not terrible.
    for (auto& slot : _glyphAtlasMap.container())
    {
        for (auto& glyphs : slot.glyphs)
        {
            glyphs.clear();
        }
    }
    for (auto& glyphs : _builtinGlyphs.glyphs)
    {
        glyphs.clear();
    }

    _d2dBeginDrawing();
    _d2dRenderTarget->Clear();

    _fontChangedResetGlyphAtlas = false;
}

void BackendD3D::_resizeGlyphAtlas(const RenderingPayload& p, const u16 u, const u16 v)
{
#if defined(_M_X64) || defined(_M_IX86)
    static const auto faultyMacTypeVersion = _checkMacTypeVersion(p);
#else
    // The affected versions of MacType are unavailable on ARM.
    static constexpr auto faultyMacTypeVersion = false;
#endif

    _d2dRenderTarget.reset();
    _d2dRenderTarget4.reset();
    _glyphAtlas.reset();
    _glyphAtlasView.reset();

    {
        const D3D11_TEXTURE2D_DESC desc{
            .Width = u,
            .Height = v,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        };
        THROW_IF_FAILED(p.device->CreateTexture2D(&desc, nullptr, _glyphAtlas.addressof()));
        THROW_IF_FAILED(p.device->CreateShaderResourceView(_glyphAtlas.get(), nullptr, _glyphAtlasView.addressof()));
    }

    {
        const auto surface = _glyphAtlas.query<IDXGISurface>();

        static constexpr D2D1_RENDER_TARGET_PROPERTIES props{
            .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
            .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
        };
        // ID2D1RenderTarget and ID2D1DeviceContext are the same and I'm tired of pretending they're not.
        THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, reinterpret_cast<ID2D1RenderTarget**>(_d2dRenderTarget.addressof())));
        _d2dRenderTarget.try_query_to(_d2dRenderTarget4.addressof());

        _d2dRenderTarget->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        // Ensure that D2D uses the exact same gamma as our shader uses.
        _d2dRenderTarget->SetTextRenderingParams(_textRenderingParams.get());

        _d2dRenderTargetUpdateFontSettings(p);
    }

    // We have our own glyph cache so Direct2D's cache doesn't help much.
    // This saves us 1MB of RAM, which is not much, but also not nothing.
    if (_d2dRenderTarget4)
    {
        wil::com_ptr<ID2D1Device> device;
        _d2dRenderTarget4->GetDevice(device.addressof());

        device->SetMaximumTextureMemory(0);

        if (!faultyMacTypeVersion)
        {
            if (const auto device4 = device.try_query<ID2D1Device4>())
            {
                device4->SetMaximumColorGlyphCacheMemory(0);
            }
        }
    }

    {
        THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&whiteColor, nullptr, _emojiBrush.put()));
        THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&whiteColor, nullptr, _brush.put()));
    }

    ID3D11ShaderResourceView* resources[]{ _backgroundBitmapView.get(), _glyphAtlasView.get() };
    p.deviceContext->PSSetShaderResources(0, 2, &resources[0]);

    _rectPackerData = Buffer<stbrp_node>{ u };
}

// MacType is a popular 3rd party system to give the font rendering on Windows a softer look.
// It's particularly popular in China. Unfortunately, it hooks ID2D1Device4 incorrectly:
//   https://github.com/snowie2000/mactype/pull/938
// This results in crashes. Not a lot of them, but enough to constantly show up.
// The issue was fixed in the MacType v1.2023.5.31 release, the only one in 2023.
//
// Please feel free to remove this check in a few years.
bool BackendD3D::_checkMacTypeVersion(const RenderingPayload& p)
{
#ifdef _WIN64
    static constexpr auto name = L"MacType64.Core.dll";
#else
    static constexpr auto name = L"MacType.Core.dll";
#endif

    wil::unique_hmodule handle;
    if (!GetModuleHandleExW(0, name, handle.addressof()))
    {
        return false;
    }

    const auto resource = FindResourceW(handle.get(), MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
    if (!resource)
    {
        return false;
    }

    const auto dataHandle = LoadResource(handle.get(), resource);
    if (!dataHandle)
    {
        return false;
    }

    const auto data = LockResource(dataHandle);
    if (!data)
    {
        return false;
    }

    VS_FIXEDFILEINFO* info;
    UINT varLen = 0;
    if (!VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&info), &varLen))
    {
        return false;
    }

    const auto faulty = info->dwFileVersionMS < (1 << 16 | 2023);

    if (faulty && p.warningCallback)
    {
        p.warningCallback(ATLAS_ENGINE_ERROR_MAC_TYPE, {});
    }

    return faulty;
}

void BackendD3D::_appendQuad()
{
}

void BackendD3D::_flushQuads(const RenderingPayload& p)
{
    if (!_cellBuffer)
    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(Cell) * 120 * 30,
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
            .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            .StructureByteStride = sizeof(Cell),
        };
        THROW_IF_FAILED(p.device->CreateBuffer(&desc, nullptr, _cellBuffer.addressof()));
        THROW_IF_FAILED(p.device->CreateShaderResourceView(_cellBuffer.get(), nullptr, _cellBufferView.addressof()));
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        THROW_IF_FAILED(p.deviceContext->Map(_cellBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        assert(mapped.RowPitch >= sizeof(_cells));
        memcpy(mapped.pData, _cells, sizeof(_cells));
        p.deviceContext->Unmap(_cellBuffer.get(), 0);
    }

    p.deviceContext->CSSetConstantBuffers(0, 1, _constantBuffer.addressof());
    ID3D11ShaderResourceView* resources[]{ _glyphAtlasView.get(), _cellBufferView.get() };
    p.deviceContext->CSSetShaderResources(0, ARRAYSIZE(resources), &resources[0]);
    p.deviceContext->CSSetUnorderedAccessViews(0, 1, _renderTargetView.addressof(), nullptr);
    p.deviceContext->CSSetShader(_computeShader.get(), nullptr, 0);
    p.deviceContext->Dispatch((p.s->targetSize.x + 7) / 8, (p.s->targetSize.x + 7) / 8, 1);
}

void BackendD3D::_drawBackground(const RenderingPayload& p)
{
    // Not uploading the bitmap halves (!) the GPU load for any given frame on 2023 hardware.
    if (_backgroundBitmapGeneration != p.colorBitmapGenerations[0])
    {
        _uploadBackgroundBitmap(p);
    }

    for (int y = 0; y < 30; y++)
    {
        for (int x = 0; x < 120; x++)
        {
            _cells[y][x].background = p.backgroundBitmap[y * 120 + x];
            _cells[y][x].foreground = p.foregroundBitmap[y * 120 + x];
            _cells[y][x].glyphX = (u32)-1;
            _cells[y][x].glyphY = (u32)-1;
        }
    }
}

void BackendD3D::_uploadBackgroundBitmap(const RenderingPayload& p)
{
    _backgroundBitmapGeneration = p.colorBitmapGenerations[0];
}

void BackendD3D::_drawText(RenderingPayload& p)
{
    if (_fontChangedResetGlyphAtlas)
    {
        _resetGlyphAtlas(p);
    }

    til::CoordType dirtyTop = til::CoordTypeMax;
    til::CoordType dirtyBottom = til::CoordTypeMin;

    u16 y = 0;
    for (const auto row : p.rows)
    {
        f32 baselineX = 0;
        f32 baselineY = y * p.s->font->cellSize.y + p.s->font->baseline;
        f32 scaleX = 1;
        f32 scaleY = 1;

        if (row->lineRendition != LineRendition::SingleWidth)
        {
            scaleX = 2;

            if (row->lineRendition >= LineRendition::DoubleHeightTop)
            {
                scaleY = 2;
                baselineY /= 2;
            }
        }

        const u8x2 renditionScale{
            static_cast<u8>(row->lineRendition != LineRendition::SingleWidth ? 2 : 1),
            static_cast<u8>(row->lineRendition >= LineRendition::DoubleHeightTop ? 2 : 1),
        };

        for (const auto& m : row->mappings)
        {
            auto x = m.glyphsFrom;
            const auto glyphsTo = m.glyphsTo;
            const auto fontFace = m.fontFace.get();

            // The lack of a fontFace indicates a soft font.
            AtlasFontFaceEntry* fontFaceEntry = &_builtinGlyphs;
            if (fontFace) [[likely]]
            {
                fontFaceEntry = _glyphAtlasMap.insert(fontFace).first;
            }

            const auto& glyphs = fontFaceEntry->glyphs[WI_EnumValue(row->lineRendition)];

            while (x < glyphsTo)
            {
                size_t dx = 1;
                u32 glyphIndex = row->glyphIndices[x];

                // Note: !fontFace is only nullptr for builtin glyphs which then use glyphIndices for UTF16 code points.
                // In other words, this doesn't accidentally corrupt any actual glyph indices.
                if (!fontFace && til::is_leading_surrogate(glyphIndex))
                {
                    glyphIndex = til::combine_surrogates(glyphIndex, row->glyphIndices[x + 1]);
                    dx = 2;
                }

                auto glyphEntry = glyphs.lookup(glyphIndex);
                if (!glyphEntry)
                {
                    glyphEntry = _drawGlyph(p, *row, *fontFaceEntry, glyphIndex);
                }

                // A shadingType of 0 (ShadingType::Default) indicates a glyph that is whitespace.
                if (glyphEntry->shadingType != ShadingType::Default)
                {
                    auto l = static_cast<til::CoordType>(lrintf((baselineX + row->glyphOffsets[x].advanceOffset) * scaleX));
                    auto t = static_cast<til::CoordType>(lrintf((baselineY - row->glyphOffsets[x].ascenderOffset) * scaleY));

                    l += glyphEntry->offset.x;
                    t += glyphEntry->offset.y;

                    row->dirtyTop = std::min(row->dirtyTop, t);
                    row->dirtyBottom = std::max(row->dirtyBottom, t + glyphEntry->size.y);

                    const auto cx = (l + p.s->font->cellSize.x / 2) / p.s->font->cellSize.x;
                    const auto cy = (t + p.s->font->cellSize.y / 2) / p.s->font->cellSize.y;
                    _cells[cy][cx].glyphX = glyphEntry->texcoord.x;
                    _cells[cy][cx].glyphY = glyphEntry->texcoord.y;

                    if (glyphEntry->overlapSplit)
                    {
                        _drawTextOverlapSplit(p, y);
                    }
                }

                baselineX += row->glyphAdvances[x];
                x += dx;
            }
        }

        if (!row->gridLineRanges.empty())
        {
            _drawGridlines(p, y);
        }

        if (p.invalidatedRows.contains(y))
        {
            dirtyTop = std::min(dirtyTop, row->dirtyTop);
            dirtyBottom = std::max(dirtyBottom, row->dirtyBottom);
        }

        ++y;
    }

    if (dirtyTop < dirtyBottom)
    {
        p.dirtyRectInPx.top = std::min(p.dirtyRectInPx.top, dirtyTop);
        p.dirtyRectInPx.bottom = std::max(p.dirtyRectInPx.bottom, dirtyBottom);
    }

    _d2dEndDrawing();
}

// There are a number of coding-oriented fonts that feature ligatures which (for instance)
// translate text like "!=" into a glyph that looks like "≠" (just 2 columns wide and not 1).
// Glyphs like that still need to be colored in potentially multiple colors however, so this
// function will handle these ligatures by splitting them up into multiple QuadInstances.
//
// It works by iteratively splitting the wide glyph into shorter and shorter segments like so
// (whitespaces indicate that the glyph was split up in a leading and trailing half):
//   <!--
//   < !--
//   < ! --
//   < ! - -
void BackendD3D::_drawTextOverlapSplit(const RenderingPayload& p, u16 y)
{
}

BackendD3D::AtlasGlyphEntry* BackendD3D::_drawGlyph(const RenderingPayload& p, const ShapedRow& row, AtlasFontFaceEntry& fontFaceEntry, u32 glyphIndex)
{
    // The lack of a fontFace indicates a soft font.
    if (!fontFaceEntry.fontFace)
    {
        return _drawBuiltinGlyph(p, row, fontFaceEntry, glyphIndex);
    }

    const auto glyphIndexU16 = static_cast<u16>(glyphIndex);
    const DWRITE_GLYPH_RUN glyphRun{
        .fontFace = fontFaceEntry.fontFace.get(),
        .fontEmSize = p.s->font->fontSize,
        .glyphCount = 1,
        .glyphIndices = &glyphIndexU16,
    };

    // It took me a while to figure out how to rasterize glyphs manually with DirectWrite without depending on Direct2D.
    // The benefits are a reduction in memory usage, an increase in performance under certain circumstances and most
    // importantly, the ability to debug the renderer more easily, because many graphics debuggers don't support Direct2D.
    // Direct2D has one big advantage however: It features a clever glyph uploader with a pool of D3D11_USAGE_STAGING textures,
    // which I was too short on time to implement myself. This makes rasterization with Direct2D roughly 2x faster.
    //
    // This code remains, because it features some parts that are slightly more and some parts that are outright difficult to figure out.
#if 0
    const auto wantsClearType = p.s->font->antialiasingMode == AntialiasingMode::ClearType;
    const auto wantsAliased = p.s->font->antialiasingMode == AntialiasingMode::Aliased;
    const auto antialiasMode = wantsClearType ? DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE : DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    const auto outlineThreshold = wantsAliased ? DWRITE_OUTLINE_THRESHOLD_ALIASED : DWRITE_OUTLINE_THRESHOLD_ANTIALIASED;

    DWRITE_RENDERING_MODE renderingMode{};
    DWRITE_GRID_FIT_MODE gridFitMode{};
    THROW_IF_FAILED(fontFaceEntry.fontFace->GetRecommendedRenderingMode(
        /* fontEmSize       */ glyphRun.fontEmSize,
        /* dpiX             */ 1, // fontEmSize is already in pixel
        /* dpiY             */ 1, // fontEmSize is already in pixel
        /* transform        */ nullptr,
        /* isSideways       */ FALSE,
        /* outlineThreshold */ outlineThreshold,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* renderingParams  */ _textRenderingParams.get(),
        /* renderingMode    */ &renderingMode,
        /* gridFitMode      */ &gridFitMode));

    wil::com_ptr<IDWriteGlyphRunAnalysis> glyphRunAnalysis;
    THROW_IF_FAILED(p.dwriteFactory->CreateGlyphRunAnalysis(
        /* glyphRun         */ &glyphRun,
        /* transform        */ nullptr,
        /* renderingMode    */ renderingMode,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* gridFitMode      */ gridFitMode,
        /* antialiasMode    */ antialiasMode,
        /* baselineOriginX  */ 0,
        /* baselineOriginY  */ 0,
        /* glyphRunAnalysis */ glyphRunAnalysis.put()));

    RECT textureBounds{};

    if (wantsClearType)
    {
        THROW_IF_FAILED(glyphRunAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds));

        // Some glyphs cannot be drawn with ClearType, such as bitmap fonts. In that case
        // GetAlphaTextureBounds() supposedly returns an empty RECT, but I haven't tested that yet.
        if (!IsRectEmpty(&textureBounds))
        {
            // Allocate a buffer of `3 * width * height` bytes.
            THROW_IF_FAILED(glyphRunAnalysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds, buffer.data(), buffer.size()));
            // The buffer contains RGB ClearType weights which can now be packed into RGBA and uploaded to the glyph atlas.
            return;
        }

        // --> Retry with grayscale AA.
    }

    // Even though it says "ALIASED" and even though the docs for the flag still say:
    // > [...] that is, each pixel is either fully opaque or fully transparent [...]
    // don't be confused: It's grayscale antialiased. lol
    THROW_IF_FAILED(glyphRunAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &textureBounds));

    // Allocate a buffer of `width * height` bytes.
    THROW_IF_FAILED(glyphRunAnalysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &textureBounds, buffer.data(), buffer.size()));
    // The buffer now contains a grayscale alpha mask.
#endif

    // This code finds the local font file path. Useful for debugging as it
    // gets you the font.ttf <> glyphIndex pair to uniquely identify glyphs.
#if 0
    std::vector<std::wstring> paths;

    UINT32 numberOfFiles;
    THROW_IF_FAILED(fontFaceEntry.fontFace->GetFiles(&numberOfFiles, nullptr));
    wil::com_ptr<IDWriteFontFile> fontFiles[8];
    THROW_IF_FAILED(fontFaceEntry.fontFace->GetFiles(&numberOfFiles, fontFiles[0].addressof()));

    for (UINT32 i = 0; i < numberOfFiles; ++i)
    {
        wil::com_ptr<IDWriteFontFileLoader> loader;
        THROW_IF_FAILED(fontFiles[i]->GetLoader(loader.addressof()));

        void const* fontFileReferenceKey;
        UINT32 fontFileReferenceKeySize;
        THROW_IF_FAILED(fontFiles[i]->GetReferenceKey(&fontFileReferenceKey, &fontFileReferenceKeySize));

        if (const auto localLoader = loader.try_query<IDWriteLocalFontFileLoader>())
        {
            UINT32 filePathLength;
            THROW_IF_FAILED(localLoader->GetFilePathLengthFromKey(fontFileReferenceKey, fontFileReferenceKeySize, &filePathLength));

            filePathLength++;
            std::wstring filePath(filePathLength, L'\0');
            THROW_IF_FAILED(localLoader->GetFilePathFromKey(fontFileReferenceKey, fontFileReferenceKeySize, filePath.data(), filePathLength));

            paths.emplace_back(std::move(filePath));
        }
    }
#endif

    const int scale = row.lineRendition != LineRendition::SingleWidth;
    D2D1_MATRIX_3X2_F transform = identityTransform;

    if (scale)
    {
        transform.m11 = 2.0f;
        transform.m22 = row.lineRendition >= LineRendition::DoubleHeightTop ? 2.0f : 1.0f;
        _d2dRenderTarget->SetTransform(&transform);
    }

    const auto restoreTransform = wil::scope_exit([&]() noexcept {
        _d2dRenderTarget->SetTransform(&identityTransform);
    });

    // This calculates the black box of the glyph, or in other words,
    // it's extents/size relative to its baseline origin (at 0,0).
    //
    // bounds.top ------++-----######--+
    //   (-7)           ||  ############
    //                  ||####      ####
    //                  |###       #####
    //  baseline ______ |###      #####|
    //   origin        \|############# |
    //  (= 0,0)         \|###########  |
    //                  ++-------###---+
    //                  ##      ###    |
    // bounds.bottom ---+#########-----+
    //    (+2)          |              |
    //             bounds.left     bounds.right
    //                 (-1)           (+14)
    //

    bool isColorGlyph = false;
    D2D1_RECT_F bounds = GlyphRunEmptyBounds;

    const auto antialiasingCleanup = wil::scope_exit([&]() {
        if (isColorGlyph)
        {
            _d2dRenderTarget4->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->font->antialiasingMode));
        }
    });

    {
        wil::com_ptr<IDWriteColorGlyphRunEnumerator1> enumerator;

        if (p.s->font->colorGlyphs)
        {
            enumerator = TranslateColorGlyphRun(p.dwriteFactory4.get(), {}, &glyphRun);
        }

        if (!enumerator)
        {
            THROW_IF_FAILED(_d2dRenderTarget->GetGlyphRunWorldBounds({}, &glyphRun, DWRITE_MEASURING_MODE_NATURAL, &bounds));
        }
        else
        {
            isColorGlyph = true;
            _d2dRenderTarget4->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

            while (ColorGlyphRunMoveNext(enumerator.get()))
            {
                const auto colorGlyphRun = ColorGlyphRunGetCurrentRun(enumerator.get());
                ColorGlyphRunAccumulateBounds(_d2dRenderTarget.get(), colorGlyphRun, bounds);
            }
        }
    }

    // The bounds may be empty if the glyph is whitespace.
    if (bounds.left >= bounds.right || bounds.top >= bounds.bottom)
    {
        return _drawGlyphAllocateEntry(row, fontFaceEntry, glyphIndex);
    }

    const auto bl = lrintf(bounds.left);
    const auto bt = lrintf(bounds.top);
    const auto br = lrintf(bounds.right);
    const auto bb = lrintf(bounds.bottom);

    stbrp_rect rect{
        .w = br - bl,
        .h = bb - bt,
    };
    _drawGlyphAtlasAllocate(p, rect);
    _d2dBeginDrawing();

    const D2D1_POINT_2F baselineOrigin{
        static_cast<f32>(rect.x - bl),
        static_cast<f32>(rect.y - bt),
    };

    if (scale)
    {
        transform.dx = (1.0f - transform.m11) * baselineOrigin.x;
        transform.dy = (1.0f - transform.m22) * baselineOrigin.y;
        _d2dRenderTarget->SetTransform(&transform);
    }

    if (!isColorGlyph)
    {
        _d2dRenderTarget->DrawGlyphRun(baselineOrigin, &glyphRun, _brush.get(), DWRITE_MEASURING_MODE_NATURAL);
    }
    else
    {
        const auto enumerator = TranslateColorGlyphRun(p.dwriteFactory4.get(), baselineOrigin, &glyphRun);
        while (ColorGlyphRunMoveNext(enumerator.get()))
        {
            const auto colorGlyphRun = ColorGlyphRunGetCurrentRun(enumerator.get());
            ColorGlyphRunDraw(_d2dRenderTarget4.get(), _emojiBrush.get(), _brush.get(), colorGlyphRun);
        }
    }

    // Ligatures are drawn with strict cell-wise foreground color, while other text allows colors to overhang
    // their cells. This makes sure that italics and such retain their color and don't look "cut off".
    //
    // The former condition makes sure to exclude diacritics and such from being considered a ligature,
    // while the latter condition-pair makes sure to exclude regular BMP wide glyphs that overlap a little.
    const auto triggerLeft = _ligatureOverhangTriggerLeft << scale;
    const auto triggerRight = _ligatureOverhangTriggerRight << scale;
    const auto overlapSplit = rect.w >= p.s->font->cellSize.x && (bl <= triggerLeft || br >= triggerRight);

    const auto glyphEntry = _drawGlyphAllocateEntry(row, fontFaceEntry, glyphIndex);
    glyphEntry->shadingType = isColorGlyph ? ShadingType::TextPassthrough : _textShadingType;
    glyphEntry->overlapSplit = overlapSplit;
    glyphEntry->offset.x = bl;
    glyphEntry->offset.y = bt;
    glyphEntry->size.x = rect.w;
    glyphEntry->size.y = rect.h;
    glyphEntry->texcoord.x = rect.x;
    glyphEntry->texcoord.y = rect.y;

    if (row.lineRendition >= LineRendition::DoubleHeightTop)
    {
        _splitDoubleHeightGlyph(p, row, fontFaceEntry, glyphEntry);
    }

    return glyphEntry;
}

BackendD3D::AtlasGlyphEntry* BackendD3D::_drawBuiltinGlyph(const RenderingPayload& p, const ShapedRow& row, AtlasFontFaceEntry& fontFaceEntry, u32 glyphIndex)
{
    auto baseline = p.s->font->baseline;
    stbrp_rect rect{
        .w = p.s->font->cellSize.x,
        .h = p.s->font->cellSize.y,
    };
    if (row.lineRendition != LineRendition::SingleWidth)
    {
        const auto heightShift = static_cast<u8>(row.lineRendition >= LineRendition::DoubleHeightTop);
        rect.w <<= 1;
        rect.h <<= heightShift;
        baseline <<= heightShift;
    }

    _drawGlyphAtlasAllocate(p, rect);
    _d2dBeginDrawing();

    auto shadingType = ShadingType::TextGrayscale;
    const D2D1_RECT_F r{
        static_cast<f32>(rect.x),
        static_cast<f32>(rect.y),
        static_cast<f32>(rect.x + rect.w),
        static_cast<f32>(rect.y + rect.h),
    };

    if (BuiltinGlyphs::IsSoftFontChar(glyphIndex))
    {
        shadingType = _drawSoftFontGlyph(p, r, glyphIndex);
    }
    else
    {
        // This code works in tandem with SHADING_TYPE_TEXT_BUILTIN_GLYPH in our pixel shader.
        // Unless someone removed it, it should have a lengthy comment visually explaining
        // what each of the 3 RGB components do. The short version is:
        //   R: stretch the checkerboard pattern (Shape_Filled050) horizontally
        //   G: invert the pixels
        //   B: overrides the above and fills it
        static constexpr D2D1_COLOR_F shadeColorMap[] = {
            { 1, 0, 0, 1 }, // Shape_Filled025
            { 0, 0, 0, 1 }, // Shape_Filled050
            { 1, 1, 0, 1 }, // Shape_Filled075
            { 1, 1, 1, 1 }, // Shape_Filled100
        };
        BuiltinGlyphs::DrawBuiltinGlyph(p.d2dFactory.get(), _d2dRenderTarget.get(), _brush.get(), shadeColorMap, r, glyphIndex);
        shadingType = ShadingType::TextBuiltinGlyph;
    }

    const auto glyphEntry = _drawGlyphAllocateEntry(row, fontFaceEntry, glyphIndex);
    glyphEntry->shadingType = shadingType;
    glyphEntry->overlapSplit = 0;
    glyphEntry->offset.x = 0;
    glyphEntry->offset.y = -baseline;
    glyphEntry->size.x = rect.w;
    glyphEntry->size.y = rect.h;
    glyphEntry->texcoord.x = rect.x;
    glyphEntry->texcoord.y = rect.y;

    if (row.lineRendition >= LineRendition::DoubleHeightTop)
    {
        _splitDoubleHeightGlyph(p, row, fontFaceEntry, glyphEntry);
    }

    return glyphEntry;
}

BackendD3D::ShadingType BackendD3D::_drawSoftFontGlyph(const RenderingPayload& p, const D2D1_RECT_F& rect, u32 glyphIndex)
{
    const auto width = static_cast<size_t>(p.s->font->softFontCellSize.width);
    const auto height = static_cast<size_t>(p.s->font->softFontCellSize.height);
    const auto softFontIndex = glyphIndex - 0xEF20u;
    const auto data = til::safe_slice_len(p.s->font->softFontPattern, height * softFontIndex, height);

    // This happens if someone wrote a U+EF2x character (by accident), but we don't even have soft fonts enabled yet.
    if (data.empty() || data.size() != height)
    {
        return ShadingType::Default;
    }

    if (!_softFontBitmap)
    {
        // Allocating such a tiny texture is very wasteful (min. texture size on GPUs
        // right now is 64kB), but this is a seldom used feature, so it's fine...
        const D2D1_SIZE_U size{
            static_cast<UINT32>(width),
            static_cast<UINT32>(height),
        };
        const D2D1_BITMAP_PROPERTIES1 bitmapProperties{
            .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            .dpiX = static_cast<f32>(p.s->font->dpi),
            .dpiY = static_cast<f32>(p.s->font->dpi),
        };
        THROW_IF_FAILED(_d2dRenderTarget->CreateBitmap(size, nullptr, 0, &bitmapProperties, _softFontBitmap.addressof()));
    }

    {
        auto bitmapData = Buffer<u32>{ width * height };
        auto dst = bitmapData.begin();

        for (auto srcBits : data)
        {
            for (size_t x = 0; x < width; x++)
            {
                const auto srcBitIsSet = (srcBits & 0x8000) != 0;
                *dst++ = srcBitIsSet ? 0xffffffff : 0x00000000;
                srcBits <<= 1;
            }
        }

        const auto pitch = static_cast<UINT32>(width * sizeof(u32));
        THROW_IF_FAILED(_softFontBitmap->CopyFromMemory(nullptr, bitmapData.data(), pitch));
    }

    _d2dRenderTarget->PushAxisAlignedClip(&rect, D2D1_ANTIALIAS_MODE_ALIASED);
    const auto restoreD2D = wil::scope_exit([&]() {
        _d2dRenderTarget->PopAxisAlignedClip();
    });

    const auto interpolation = p.s->font->antialiasingMode == AntialiasingMode::Aliased ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    _d2dRenderTarget->DrawBitmap(_softFontBitmap.get(), &rect, 1, interpolation, nullptr, nullptr);
    return ShadingType::TextGrayscale;
}

void BackendD3D::_drawGlyphAtlasAllocate(const RenderingPayload& p, stbrp_rect& rect)
{
    if (stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        return;
    }

    _d2dEndDrawing();
<<<<<<< Updated upstream
    _flushQuads(p);
    _resetGlyphAtlas(p);
||||||| Stash base
    _flushQuads(p);
    _resetGlyphAtlas(p, rect.w, rect.h);
=======
    _resetGlyphAtlas(p, rect.w, rect.h);
>>>>>>> Stashed changes

    if (!stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        THROW_HR(HRESULT_FROM_WIN32(ERROR_POSSIBLE_DEADLOCK));
    }
}

BackendD3D::AtlasGlyphEntry* BackendD3D::_drawGlyphAllocateEntry(const ShapedRow& row, AtlasFontFaceEntry& fontFaceEntry, u32 glyphIndex)
{
    const auto glyphEntry = fontFaceEntry.glyphs[WI_EnumValue(row.lineRendition)].insert(glyphIndex).first;
    glyphEntry->shadingType = ShadingType::Default;
    return glyphEntry;
}

// If this is a double-height glyph (DECDHL), we need to split it into 2 glyph entries:
// One for the top/bottom half each, because that's how DECDHL works. This will clip the
// `glyphEntry` to only contain the one specified by `fontFaceEntry.lineRendition`
// and create a second entry in our glyph cache hashmap that contains the other half.
void BackendD3D::_splitDoubleHeightGlyph(const RenderingPayload& p, const ShapedRow& row, AtlasFontFaceEntry& fontFaceEntry, AtlasGlyphEntry* glyphEntry)
{
    // Twice the line height, twice the descender gap. For both.
    glyphEntry->offset.y -= p.s->font->descender;

    const auto isTop = row.lineRendition == LineRendition::DoubleHeightTop;
    const auto otherLineRendition = isTop ? LineRendition::DoubleHeightBottom : LineRendition::DoubleHeightTop;
    const auto entry2 = fontFaceEntry.glyphs[WI_EnumValue(otherLineRendition)].insert(glyphEntry->glyphIndex).first;

    *entry2 = *glyphEntry;

    const auto top = isTop ? glyphEntry : entry2;
    const auto bottom = isTop ? entry2 : glyphEntry;
    const auto topSize = clamp(-glyphEntry->offset.y - p.s->font->baseline, 0, static_cast<int>(glyphEntry->size.y));

    top->offset.y += p.s->font->cellSize.y;
    top->size.y = topSize;
    bottom->offset.y += topSize;
    bottom->size.y = std::max(0, bottom->size.y - topSize);
    bottom->texcoord.y += topSize;

    // Things like diacritics might be so small that they only exist on either half of the
    // double-height row. This effectively turns the other (unneeded) side into whitespace.
    if (!top->size.y)
    {
        top->shadingType = ShadingType::Default;
    }
    if (!bottom->size.y)
    {
        bottom->shadingType = ShadingType::Default;
    }
}

void BackendD3D::_drawGridlines(const RenderingPayload& p, u16 y)
{
<<<<<<< Updated upstream
    const auto row = p.rows[y];

    const auto horizontalShift = static_cast<u8>(row->lineRendition != LineRendition::SingleWidth);
    const auto verticalShift = static_cast<u8>(row->lineRendition >= LineRendition::DoubleHeightTop);

    const auto cellSize = p.s->font->cellSize;
    const auto rowTop = static_cast<i16>(cellSize.y * y);
    const auto rowBottom = static_cast<i16>(rowTop + cellSize.y);

    auto textCellTop = rowTop;
    if (row->lineRendition == LineRendition::DoubleHeightBottom)
    {
        textCellTop -= cellSize.y;
    }

    const i32 clipTop = row->lineRendition == LineRendition::DoubleHeightBottom ? rowTop : 0;
    const i32 clipBottom = row->lineRendition == LineRendition::DoubleHeightTop ? rowBottom : p.s->targetSize.y;

    const auto appendVerticalLines = [&](const GridLineRange& r, FontDecorationPosition pos) {
        const auto textCellWidth = cellSize.x << horizontalShift;
        const auto offset = pos.position << horizontalShift;
        const auto width = static_cast<u16>(pos.height << horizontalShift);

        auto posX = r.from * cellSize.x + offset;
        const auto end = r.to * cellSize.x;

        for (; posX < end; posX += textCellWidth)
        {
            _appendQuad() = {
                .shadingType = static_cast<u16>(ShadingType::SolidLine),
                .position = { static_cast<i16>(posX), rowTop },
                .size = { width, p.s->font->cellSize.y },
                .color = r.gridlineColor,
            };
        }
    };
    const auto appendHorizontalLine = [&](const GridLineRange& r, FontDecorationPosition pos, ShadingType shadingType, const u32 color) {
        const auto offset = pos.position << verticalShift;
        const auto height = static_cast<u16>(pos.height << verticalShift);

        const auto left = static_cast<i16>(r.from * cellSize.x);
        const auto width = static_cast<u16>((r.to - r.from) * cellSize.x);

        i32 rt = textCellTop + offset;
        i32 rb = rt + height;
        rt = clamp(rt, clipTop, clipBottom);
        rb = clamp(rb, clipTop, clipBottom);

        if (rt < rb)
        {
            _appendQuad() = {
                .shadingType = static_cast<u16>(shadingType),
                .renditionScale = { static_cast<u8>(1 << horizontalShift), static_cast<u8>(1 << verticalShift) },
                .position = { left, static_cast<i16>(rt) },
                .size = { width, static_cast<u16>(rb - rt) },
                .color = color,
            };
        }
    };

    for (const auto& r : row->gridLineRanges)
    {
        // AtlasEngine.cpp shouldn't add any gridlines if they don't do anything.
        assert(r.lines.any());

        if (r.lines.test(GridLines::Left))
        {
            appendVerticalLines(r, p.s->font->gridLeft);
        }
        if (r.lines.test(GridLines::Right))
        {
            appendVerticalLines(r, p.s->font->gridRight);
        }
        if (r.lines.test(GridLines::Top))
        {
            appendHorizontalLine(r, p.s->font->gridTop, ShadingType::SolidLine, r.gridlineColor);
        }
        if (r.lines.test(GridLines::Bottom))
        {
            appendHorizontalLine(r, p.s->font->gridBottom, ShadingType::SolidLine, r.gridlineColor);
        }
        if (r.lines.test(GridLines::Strikethrough))
        {
            appendHorizontalLine(r, p.s->font->strikethrough, ShadingType::SolidLine, r.gridlineColor);
        }

        if (r.lines.test(GridLines::Underline))
        {
            appendHorizontalLine(r, p.s->font->underline, ShadingType::SolidLine, r.underlineColor);
        }
        else if (r.lines.any(GridLines::DottedUnderline, GridLines::HyperlinkUnderline))
        {
            appendHorizontalLine(r, p.s->font->underline, ShadingType::DottedLine, r.underlineColor);
        }
        else if (r.lines.test(GridLines::DashedUnderline))
        {
            appendHorizontalLine(r, p.s->font->underline, ShadingType::DashedLine, r.underlineColor);
        }
        else if (r.lines.test(GridLines::CurlyUnderline))
        {
            appendHorizontalLine(r, _curlyUnderline, ShadingType::CurlyLine, r.underlineColor);
        }
        else if (r.lines.test(GridLines::DoubleUnderline))
        {
            for (const auto pos : p.s->font->doubleUnderline)
            {
                appendHorizontalLine(r, pos, ShadingType::SolidLine, r.underlineColor);
            }
        }
    }
}

||||||| Stash base
    const auto row = p.rows[y];

    const auto horizontalShift = static_cast<u8>(row->lineRendition != LineRendition::SingleWidth);
    const auto verticalShift = static_cast<u8>(row->lineRendition >= LineRendition::DoubleHeightTop);

    const auto cellSize = p.s->font->cellSize;
    const auto rowTop = static_cast<i16>(cellSize.y * y);
    const auto rowBottom = static_cast<i16>(rowTop + cellSize.y);

    auto textCellTop = rowTop;
    if (row->lineRendition == LineRendition::DoubleHeightBottom)
    {
        textCellTop -= cellSize.y;
    }

    const i32 clipTop = row->lineRendition == LineRendition::DoubleHeightBottom ? rowTop : 0;
    const i32 clipBottom = row->lineRendition == LineRendition::DoubleHeightTop ? rowBottom : p.s->targetSize.y;

    const auto appendVerticalLines = [&](const GridLineRange& r, FontDecorationPosition pos) {
        const auto textCellWidth = cellSize.x << horizontalShift;
        const auto offset = pos.position << horizontalShift;
        const auto width = static_cast<u16>(pos.height << horizontalShift);

        auto posX = r.from * cellSize.x + offset;
        const auto end = r.to * cellSize.x;

        for (; posX < end; posX += textCellWidth)
        {
            _appendQuad() = {
                .shadingType = static_cast<u16>(ShadingType::SolidLine),
                .position = { static_cast<i16>(posX), rowTop },
                .size = { width, p.s->font->cellSize.y },
                .color = r.gridlineColor,
            };
        }
    };
    const auto appendHorizontalLine = [&](const GridLineRange& r, FontDecorationPosition pos, ShadingType shadingType, const u32 color) {
        const auto offset = pos.position << verticalShift;
        const auto height = static_cast<u16>(pos.height << verticalShift);

        const auto left = static_cast<i16>(r.from * cellSize.x);
        const auto width = static_cast<u16>((r.to - r.from) * cellSize.x);

        i32 rt = textCellTop + offset;
        i32 rb = rt + height;
        rt = clamp(rt, clipTop, clipBottom);
        rb = clamp(rb, clipTop, clipBottom);

        if (rt < rb)
        {
            _appendQuad() = {
                .shadingType = static_cast<u16>(shadingType),
                .renditionScale = { static_cast<u8>(1 << horizontalShift), static_cast<u8>(1 << verticalShift) },
                .position = { left, static_cast<i16>(rt) },
                .size = { width, static_cast<u16>(rb - rt) },
                .color = color,
            };
        }
    };

    for (const auto& r : row->gridLineRanges)
    {
        // AtlasEngine.cpp shouldn't add any gridlines if they don't do anything.
        assert(r.lines.any());

        if (r.lines.test(GridLines::Left))
        {
            appendVerticalLines(r, p.s->font->gridLeft);
        }
        if (r.lines.test(GridLines::Right))
        {
            appendVerticalLines(r, p.s->font->gridRight);
        }
        if (r.lines.test(GridLines::Top))
        {
            appendHorizontalLine(r, p.s->font->gridTop, ShadingType::SolidLine, r.gridlineColor);
        }
        if (r.lines.test(GridLines::Bottom))
        {
            appendHorizontalLine(r, p.s->font->gridBottom, ShadingType::SolidLine, r.gridlineColor);
        }
        if (r.lines.test(GridLines::Strikethrough))
        {
            appendHorizontalLine(r, p.s->font->strikethrough, ShadingType::SolidLine, r.gridlineColor);
        }

        if (r.lines.test(GridLines::Underline))
        {
            appendHorizontalLine(r, p.s->font->underline, ShadingType::SolidLine, r.underlineColor);
        }
        else if (r.lines.any(GridLines::DottedUnderline, GridLines::HyperlinkUnderline))
        {
            appendHorizontalLine(r, p.s->font->underline, ShadingType::DottedLine, r.underlineColor);
        }
        else if (r.lines.test(GridLines::DashedUnderline))
        {
            appendHorizontalLine(r, p.s->font->underline, ShadingType::DashedLine, r.underlineColor);
        }
        else if (r.lines.test(GridLines::CurlyUnderline))
        {
            appendHorizontalLine(r, _curlyUnderline, ShadingType::CurlyLine, r.underlineColor);
        }
        else if (r.lines.test(GridLines::DoubleUnderline))
        {
            for (const auto pos : p.s->font->doubleUnderline)
            {
                appendHorizontalLine(r, pos, ShadingType::SolidLine, r.underlineColor);
            }
        }
    }
}

void BackendD3D::_drawBitmap(const RenderingPayload& p, const ShapedRow* row, u16 y)
{
    const auto& b = row->bitmap;
    auto ab = _glyphAtlasBitmaps.lookup(b.revision);
    if (!ab)
    {
        stbrp_rect rect{
            .w = p.s->font->cellSize.x * b.targetWidth,
            .h = p.s->font->cellSize.y,
        };
        _drawGlyphAtlasAllocate(p, rect);
        _d2dBeginDrawing();

        const D2D1_SIZE_U size{
            static_cast<UINT32>(b.sourceSize.x),
            static_cast<UINT32>(b.sourceSize.y),
        };
        const D2D1_BITMAP_PROPERTIES bitmapProperties{
            .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            .dpiX = static_cast<f32>(p.s->font->dpi),
            .dpiY = static_cast<f32>(p.s->font->dpi),
        };
        wil::com_ptr<ID2D1Bitmap> bitmap;
        THROW_IF_FAILED(_d2dRenderTarget->CreateBitmap(size, b.source.data(), static_cast<UINT32>(b.sourceSize.x) * 4, &bitmapProperties, bitmap.addressof()));

        const D2D1_RECT_F rectF{
            static_cast<f32>(rect.x),
            static_cast<f32>(rect.y),
            static_cast<f32>(rect.x + rect.w),
            static_cast<f32>(rect.y + rect.h),
        };
        _d2dRenderTarget->DrawBitmap(bitmap.get(), &rectF, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        ab = _glyphAtlasBitmaps.insert(b.revision).first;
        ab->size.x = static_cast<u16>(rect.w);
        ab->size.y = static_cast<u16>(rect.h);
        ab->texcoord.x = static_cast<u16>(rect.x);
        ab->texcoord.y = static_cast<u16>(rect.y);
    }

    const auto left = p.s->font->cellSize.x * (b.targetOffset - p.scrollOffsetX);
    const auto top = p.s->font->cellSize.y * y;

    _appendQuad() = {
        .shadingType = static_cast<u16>(ShadingType::TextPassthrough),
        .renditionScale = { 1, 1 },
        .position = { static_cast<i16>(left), static_cast<i16>(top) },
        .size = ab->size,
        .texcoord = ab->texcoord,
    };
}

=======
}

>>>>>>> Stashed changes
void BackendD3D::_drawCursorBackground(const RenderingPayload& p)
{
}

void BackendD3D::_drawCursorForeground()
{
}

size_t BackendD3D::_drawCursorForegroundSlowPath(const CursorRect& c, size_t offset)
{
    return 0;
}

void BackendD3D::_drawSelection(const RenderingPayload& p)
{
}

void BackendD3D::_debugShowDirty(const RenderingPayload& p)
{
#if ATLAS_DEBUG_SHOW_DIRTY
    _presentRects[_presentRectsPos] = p.dirtyRectInPx;
    _presentRectsPos = (_presentRectsPos + 1) % std::size(_presentRects);

    for (size_t i = 0; i < std::size(_presentRects); ++i)
    {
        const auto& rect = _presentRects[(_presentRectsPos + i) % std::size(_presentRects)];
        if (rect.non_empty())
        {
            _appendQuad() = {
                .shadingType = static_cast<u16>(ShadingType::Selection),
                .position = {
                    static_cast<i16>(rect.left),
                    static_cast<i16>(rect.top),
                },
                .size = {
                    static_cast<u16>(rect.right - rect.left),
                    static_cast<u16>(rect.bottom - rect.top),
                },
                .color = til::colorbrewer::pastel1[i] | 0x1f000000,
            };
        }
    }
#endif
}

void BackendD3D::_debugDumpRenderTarget(const RenderingPayload& p)
{
#if ATLAS_DEBUG_DUMP_RENDER_TARGET
    if (_dumpRenderTargetCounter == 0)
    {
        ExpandEnvironmentStringsW(ATLAS_DEBUG_DUMP_RENDER_TARGET_PATH, &_dumpRenderTargetBasePath[0], gsl::narrow_cast<DWORD>(std::size(_dumpRenderTargetBasePath)));
        std::filesystem::create_directories(_dumpRenderTargetBasePath);
    }

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%u_%08zu.png", &_dumpRenderTargetBasePath[0], GetCurrentProcessId(), _dumpRenderTargetCounter);
    SaveTextureToPNG(p.deviceContext.get(), _swapChainManager.GetBuffer().get(), p.s->font->dpi, &path[0]);
    _dumpRenderTargetCounter++;
#endif
}

void BackendD3D::_executeCustomShader(RenderingPayload& p)
{
    {
        // See the comment in _recreateCustomShader() which initializes the two members below and explains what they do.
        const auto now = queryPerfCount();
        const auto time = static_cast<int>(now % _customShaderPerfTickMod) * _customShaderSecsPerPerfTick;

        const CustomConstBuffer data{
            .time = time,
            .scale = static_cast<f32>(p.s->font->dpi) / static_cast<f32>(USER_DEFAULT_SCREEN_DPI),
            .resolution = {
                static_cast<f32>(_viewportCellCount.x * p.s->font->cellSize.x),
                static_cast<f32>(_viewportCellCount.y * p.s->font->cellSize.y),
            },
            .background = colorFromU32Premultiply<f32x4>(p.s->misc->backgroundColor),
        };

        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(p.deviceContext->Map(_customShaderConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, &data, sizeof(data));
        p.deviceContext->Unmap(_customShaderConstantBuffer.get(), 0);
    }

    {
        // IA: Input Assembler
        p.deviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        p.deviceContext->IASetInputLayout(nullptr);
        p.deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        p.deviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);

        // VS: Vertex Shader
        p.deviceContext->VSSetShader(_customVertexShader.get(), nullptr, 0);
        p.deviceContext->VSSetConstantBuffers(0, 0, nullptr);

        // PS: Pixel Shader
        p.deviceContext->PSSetShader(_customPixelShader.get(), nullptr, 0);
        p.deviceContext->PSSetConstantBuffers(0, 1, _customShaderConstantBuffer.addressof());
        ID3D11ShaderResourceView* const resourceViews[]{
            _customOffscreenTextureView.get(), // The terminal contents
            _customShaderTextureView.get(), // the experimental.pixelShaderImagePath, if there is one
        };
        p.deviceContext->PSSetShaderResources(0, resourceViews[1] ? 2 : 1, &resourceViews[0]);
        p.deviceContext->PSSetSamplers(0, 1, _customShaderSamplerState.addressof());

        // OM: Output Merger
        p.deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }

    p.deviceContext->Draw(4, 0);

    // With custom shaders, everything might be invalidated, so we have to
    // indirectly disable Present1() and its dirty rects this way.
    p.dirtyRectInPx = { 0, 0, p.s->targetSize.x, p.s->targetSize.y };
}

TIL_FAST_MATH_END
