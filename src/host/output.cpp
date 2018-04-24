/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "_output.h"
#include "output.h"

#include "getset.h"
#include "misc.h"
#include "../buffer/out/Ucs2CharRow.hpp"

#include "../interactivity/inc/ServiceLocator.hpp"
#include "../types/inc/Viewport.hpp"

#pragma hdrstop
using namespace Microsoft::Console::Types;

// This routine figures out what parameters to pass to CreateScreenBuffer based on the data from STARTUPINFO and the
// registry defaults, and then calls CreateScreenBuffer.
[[nodiscard]]
NTSTATUS DoCreateScreenBuffer()
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    CHAR_INFO Fill;
    Fill.Attributes = gci.GetFillAttribute();
    Fill.Char.UnicodeChar = UNICODE_SPACE;

    CHAR_INFO PopupFill;
    PopupFill.Attributes = gci.GetPopupFillAttribute();
    PopupFill.Char.UnicodeChar = UNICODE_SPACE;

    FontInfo fiFont(gci.GetFaceName(),
                    static_cast<BYTE>(gci.GetFontFamily()),
                    gci.GetFontWeight(),
                    gci.GetFontSize(),
                    gci.GetCodePage());

    // For East Asian version, we want to get the code page from the registry or shell32, so we can specify console
    // codepage by console.cpl or shell32. The default codepage is OEMCP.
    gci.CP = gci.GetCodePage();
    gci.OutputCP = gci.GetCodePage();

    gci.Flags |= CONSOLE_USE_PRIVATE_FLAGS;

    NTSTATUS Status = SCREEN_INFORMATION::CreateInstance(gci.GetWindowSize(),
                                                         fiFont,
                                                         gci.GetScreenBufferSize(),
                                                         Fill,
                                                         PopupFill,
                                                         gci.GetCursorSize(),
                                                         &gci.ScreenBuffers);

    // TODO: MSFT 9355013: This needs to be resolved. We increment it once with no handle to ensure it's never cleaned up
    // and one always exists for the renderer (and potentially other functions.)
    // It's currently a load-bearing piece of code. http://osgvsowi/9355013
    if (NT_SUCCESS(Status))
    {
        gci.ScreenBuffers[0].Header.IncrementOriginalScreenBuffer();
    }

    return Status;
}

// Routine Description:
// - This routine copies a rectangular region from the screen buffer. no clipping is done.
// Arguments:
// - screenInfo - reference to screen info
// - coordSourcePoint - upper left coordinates of source rectangle
// - TargetRect - rectangle in source buffer to copy
// Return Value:
// - vector of vector of output cell data for read rect
// Note:
// - will throw exception on error.
std::vector<std::vector<OutputCell>> ReadRectFromScreenBuffer(const SCREEN_INFORMATION& screenInfo,
                                                              const COORD coordSourcePoint,
                                                              const Viewport viewport)
{
    std::vector<std::vector<OutputCell>> result;
    result.reserve(viewport.Height());

    const int ScreenBufferWidth = screenInfo.GetScreenBufferSize().X;
    std::unique_ptr<TextAttribute[]> unpackedAttrs = std::make_unique<TextAttribute[]>(ScreenBufferWidth);
    THROW_IF_NULL_ALLOC(unpackedAttrs.get());

    for (size_t rowIndex = 0; rowIndex < static_cast<size_t>(viewport.Height()); ++rowIndex)
    {
        auto cells = screenInfo.ReadLine(coordSourcePoint.Y + rowIndex, coordSourcePoint.X);
        ASSERT_FRE(cells.size() >= static_cast<size_t>(viewport.Width()));
        for (size_t colIndex = 0; colIndex < static_cast<size_t>(viewport.Width()); ++colIndex)
        {
            // if we're clipping a dbcs char then don't include it, add a space instead
            if ((colIndex == 0 && cells[colIndex].GetDbcsAttribute().IsTrailing()) ||
                (colIndex + 1 >= static_cast<size_t>(viewport.Width()) && cells[colIndex].GetDbcsAttribute().IsLeading()))
            {
                cells[colIndex].GetDbcsAttribute().SetSingle();
                cells[colIndex].GetCharData() = { UNICODE_SPACE };
            }
        }
        cells.resize(viewport.Width(), cells.front());
        result.push_back(cells);
    }
    ASSERT_FRE(result.size() == static_cast<size_t>(viewport.Height()));
    ASSERT_FRE(result.at(0).size() == static_cast<size_t>(viewport.Width()));
    return result;
}

// Routine Description:
// - This routine copies a rectangular region from the screen buffer to the screen buffer.  no clipping is done.
// Arguments:
// - screenInfo - reference to screen info
// - sourceRect - rectangle in source buffer to copy
// - targetPoint - upper left coordinates of new location rectangle
static void _CopyRectangle(SCREEN_INFORMATION& screenInfo,
                           const SMALL_RECT sourceRect,
                           const COORD targetPoint)
{
    const COORD sourcePoint{ sourceRect.Left, sourceRect.Top };
    const SMALL_RECT targetRect{ 0, 0, sourceRect.Right - sourceRect.Left, sourceRect.Bottom - sourceRect.Top };

    std::vector<std::vector<OutputCell>> cells = ReadRectFromScreenBuffer(screenInfo,
                                                                          sourcePoint,
                                                                          Viewport::FromInclusive(targetRect));
    WriteRectToScreenBuffer(screenInfo, cells, targetPoint);
}

// Routine Description:
// - This routine reads a rectangular region from the screen buffer. The region is first clipped.
// Arguments:
// - ScreenInformation - Screen buffer to read from.
// - outputCells - an empty container to store cell data on output
// - ReadRegion - Region to read.
// Return Value:
[[nodiscard]]
NTSTATUS ReadScreenBuffer(const SCREEN_INFORMATION& screenInfo,
                          _Inout_ std::vector<std::vector<OutputCell>>& outputCells,
                          _Inout_ PSMALL_RECT psrReadRegion)
{
    DBGOUTPUT(("ReadScreenBuffer\n"));
    assert(outputCells.empty());

    // calculate dimensions of caller's buffer.  have to do this calculation before clipping.
    COORD TargetSize;
    TargetSize.X = (SHORT)(psrReadRegion->Right - psrReadRegion->Left + 1);
    TargetSize.Y = (SHORT)(psrReadRegion->Bottom - psrReadRegion->Top + 1);

    if (TargetSize.X <= 0 || TargetSize.Y <= 0)
    {
        psrReadRegion->Right = psrReadRegion->Left - 1;
        psrReadRegion->Bottom = psrReadRegion->Top - 1;
        return STATUS_SUCCESS;
    }

    // do clipping.
    const COORD coordScreenBufferSize = screenInfo.GetScreenBufferSize();
    if (psrReadRegion->Right > (SHORT)(coordScreenBufferSize.X - 1))
    {
        psrReadRegion->Right = (SHORT)(coordScreenBufferSize.X - 1);
    }
    if (psrReadRegion->Bottom > (SHORT)(coordScreenBufferSize.Y - 1))
    {
        psrReadRegion->Bottom = (SHORT)(coordScreenBufferSize.Y - 1);
    }

    COORD TargetPoint;
    if (psrReadRegion->Left < 0)
    {
        TargetPoint.X = -psrReadRegion->Left;
        psrReadRegion->Left = 0;
    }
    else
    {
        TargetPoint.X = 0;
    }

    if (psrReadRegion->Top < 0)
    {
        TargetPoint.Y = -psrReadRegion->Top;
        psrReadRegion->Top = 0;
    }
    else
    {
        TargetPoint.Y = 0;
    }

    COORD SourcePoint;
    SourcePoint.X = psrReadRegion->Left;
    SourcePoint.Y = psrReadRegion->Top;

    SMALL_RECT Target;
    Target.Left = TargetPoint.X;
    Target.Top = TargetPoint.Y;
    Target.Right = TargetPoint.X + (psrReadRegion->Right - psrReadRegion->Left);
    Target.Bottom = TargetPoint.Y + (psrReadRegion->Bottom - psrReadRegion->Top);

    try
    {
        outputCells = ReadRectFromScreenBuffer(screenInfo, SourcePoint, Viewport::FromInclusive(Target));
    }
    catch (...)
    {
        return NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
    }
    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine write a rectangular region to the screen buffer. The region is first clipped.
// - The region should contain Unicode or UnicodeOem chars.
// Arguments:
// - ScreenInformation - Screen buffer to write to.
// - Buffer - Buffer to write from.
// - ReadRegion - Region to write.
// Return Value:
[[nodiscard]]
NTSTATUS WriteScreenBuffer(SCREEN_INFORMATION& screenInfo, _In_ PCHAR_INFO pciBuffer, _Inout_ PSMALL_RECT psrWriteRegion)
{
    DBGOUTPUT(("WriteScreenBuffer\n"));

    // Calculate dimensions of caller's buffer; this calculation must be done before clipping.
    COORD SourceSize;
    SourceSize.X = (SHORT)(psrWriteRegion->Right - psrWriteRegion->Left + 1);
    SourceSize.Y = (SHORT)(psrWriteRegion->Bottom - psrWriteRegion->Top + 1);

    if (SourceSize.X <= 0 || SourceSize.Y <= 0)
    {
        return STATUS_SUCCESS;
    }

    // Ensure that the write region is within the constraints of the screen buffer.
    const COORD coordScreenBufferSize = screenInfo.GetScreenBufferSize();
    if (psrWriteRegion->Left >= coordScreenBufferSize.X || psrWriteRegion->Top >= coordScreenBufferSize.Y)
    {
        return STATUS_SUCCESS;
    }

    SMALL_RECT SourceRect;
    // Do clipping.
    if (psrWriteRegion->Right > (SHORT)(coordScreenBufferSize.X - 1))
    {
        psrWriteRegion->Right = (SHORT)(coordScreenBufferSize.X - 1);
    }
    SourceRect.Right = psrWriteRegion->Right - psrWriteRegion->Left;

    if (psrWriteRegion->Bottom > (SHORT)(coordScreenBufferSize.Y - 1))
    {
        psrWriteRegion->Bottom = (SHORT)(coordScreenBufferSize.Y - 1);
    }
    SourceRect.Bottom = psrWriteRegion->Bottom - psrWriteRegion->Top;

    if (psrWriteRegion->Left < 0)
    {
        SourceRect.Left = -psrWriteRegion->Left;
        psrWriteRegion->Left = 0;
    }
    else
    {
        SourceRect.Left = 0;
    }

    if (psrWriteRegion->Top < 0)
    {
        SourceRect.Top = -psrWriteRegion->Top;
        psrWriteRegion->Top = 0;
    }
    else
    {
        SourceRect.Top = 0;
    }

    if (SourceRect.Left > SourceRect.Right || SourceRect.Top > SourceRect.Bottom)
    {
        return STATUS_INVALID_PARAMETER;
    }

    COORD TargetPoint;
    TargetPoint.X = psrWriteRegion->Left;
    TargetPoint.Y = psrWriteRegion->Top;

    return WriteRectToScreenBuffer((PBYTE)pciBuffer, SourceSize, &SourceRect, screenInfo, TargetPoint, nullptr);
}

// Routine Description:
// - This routine reads a string of characters or attributes from the screen buffer.
// Arguments:
// - ScreenInfo - Pointer to screen buffer information.
// - Buffer - Buffer to read into.
// - ReadCoord - Screen buffer coordinate to begin reading from.
// - StringType
//      CONSOLE_ASCII         - read a string of ASCII characters.
//      CONSOLE_REAL_UNICODE  - read a string of Real Unicode characters.
//      CONSOLE_FALSE_UNICODE - read a string of False Unicode characters.
//      CONSOLE_ATTRIBUTE     - read a string of attributes.
// - NumRecords - On input, the size of the buffer in elements.  On output, the number of elements read.
// Return Value:
[[nodiscard]]
NTSTATUS ReadOutputString(const SCREEN_INFORMATION& screenInfo,
                          _Inout_ PVOID pvBuffer,
                          const COORD coordRead,
                          const ULONG ulStringType,
                          _Inout_ PULONG pcRecords)
{
    DBGOUTPUT(("ReadOutputString\n"));
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    if (*pcRecords == 0)
    {
        return STATUS_SUCCESS;
    }

    ULONG NumRead = 0;
    SHORT X = coordRead.X;
    SHORT Y = coordRead.Y;
    const COORD coordScreenBufferSize = screenInfo.GetScreenBufferSize();
    if (X >= coordScreenBufferSize.X || X < 0 || Y >= coordScreenBufferSize.Y || Y < 0)
    {
        *pcRecords = 0;
        return STATUS_SUCCESS;
    }

    PWCHAR TransBuffer = nullptr;
    PWCHAR BufPtr;
    if (ulStringType == CONSOLE_ASCII)
    {
        TransBuffer = new WCHAR[*pcRecords];
        if (TransBuffer == nullptr)
        {
            return STATUS_NO_MEMORY;
        }
        BufPtr = TransBuffer;
    }
    else
    {
        BufPtr = (PWCHAR)pvBuffer;
    }

    DbcsAttribute* TransBufferA = nullptr;
    DbcsAttribute* BufPtrA = nullptr;

    {
        TransBufferA = new DbcsAttribute[*pcRecords];
        if (TransBufferA == nullptr)
        {
            if (TransBuffer != nullptr)
            {
                delete[] TransBuffer;
            }

            return STATUS_NO_MEMORY;
        }

        BufPtrA = TransBufferA;
    }

    {
        const ROW* pRow = &screenInfo.GetTextBuffer().GetRowByOffset(coordRead.Y);
        SHORT j, k;

        if (ulStringType == CONSOLE_ASCII ||
            ulStringType == CONSOLE_REAL_UNICODE ||
            ulStringType == CONSOLE_FALSE_UNICODE)
        {
            while (NumRead < *pcRecords)
            {
                // copy the chars from its array
                try
                {
                    const ICharRow& iCharRow = pRow->GetCharRow();
                    // we only support ucs2 encoded char rows
                    FAIL_FAST_IF_MSG(iCharRow.GetSupportedEncoding() != ICharRow::SupportedEncoding::Ucs2,
                                     "only support UCS2 char rows currently");

                    const Ucs2CharRow& charRow = static_cast<const Ucs2CharRow&>(iCharRow);
                    const Ucs2CharRow::const_iterator startIt = std::next(charRow.cbegin(), X);
                    size_t copyAmount = *pcRecords - NumRead;
                    wchar_t* pChars = BufPtr;
                    DbcsAttribute* pAttrs = BufPtrA;
                    if (static_cast<size_t>(coordScreenBufferSize.X - X) > copyAmount)
                    {
                        std::for_each(startIt, std::next(startIt, copyAmount), [&](const auto& vals)
                        {
                            *pChars = vals.first;
                            ++pChars;
                            *pAttrs = vals.second;
                            ++pAttrs;
                        });

                        NumRead += static_cast<ULONG>(copyAmount);
                        break;
                    }
                    else
                    {
                        copyAmount = coordScreenBufferSize.X - X;

                        std::for_each(startIt, std::next(startIt, copyAmount), [&](const auto& vals)
                        {
                            *pChars = vals.first;
                            ++pChars;
                            *pAttrs = vals.second;
                            ++pAttrs;
                        });

                        BufPtr += copyAmount;
                        BufPtrA += copyAmount;

                        NumRead += static_cast<ULONG>(copyAmount);
                        pRow = &screenInfo.GetTextBuffer().GetNextRowNoWrap(*pRow);
                    }
                }
                catch (...)
                {
                    return NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
                }

                X = 0;
                Y++;
                if (Y >= coordScreenBufferSize.Y)
                {
                    break;
                }
            }

            if (NumRead)
            {
                PWCHAR Char;
                if (ulStringType == CONSOLE_ASCII)
                {
                    Char = BufPtr = TransBuffer;
                }
                else
                {
                    Char = BufPtr = (PWCHAR)pvBuffer;
                }

                BufPtrA = TransBufferA;
#pragma prefast(suppress:__WARNING_BUFFER_OVERFLOW, "This code is fine")
                if (BufPtrA->IsTrailing())
                {
                    j = k = (SHORT)(NumRead - 1);
                    BufPtr++;
                    *Char++ = UNICODE_SPACE;
                    BufPtrA++;
                    NumRead = 1;
                }
                else
                {
                    j = k = (SHORT)NumRead;
                    NumRead = 0;
                }

                while (j--)
                {
                    if (!BufPtrA->IsTrailing())
                    {
                        *Char++ = *BufPtr;
                        NumRead++;
                    }
                    BufPtr++;
                    BufPtrA++;
                }

                if (k && (BufPtrA - 1)->IsLeading())
                {
                    *(Char - 1) = UNICODE_SPACE;
                }
            }
        }
        else if (ulStringType == CONSOLE_ATTRIBUTE)
        {
            size_t CountOfAttr = 0;
            TextAttribute Attr;
            PWORD TargetPtr = (PWORD)BufPtr;

            while (NumRead < *pcRecords)
            {
                // Copy the attrs from its array.
                Ucs2CharRow::const_iterator it;
                Ucs2CharRow::const_iterator itEnd;
                try
                {
                    const ICharRow& iCharRow = pRow->GetCharRow();
                    // we only support ucs2 encoded char rows
                    FAIL_FAST_IF_MSG(iCharRow.GetSupportedEncoding() != ICharRow::SupportedEncoding::Ucs2,
                                     "only support UCS2 char rows currently");

                    const Ucs2CharRow& charRow = static_cast<const Ucs2CharRow&>(iCharRow);
                    it = std::next(charRow.cbegin(), X);
                    itEnd = charRow.cend();
                }
                catch (...)
                {
                    return NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
                }

                Attr = pRow->GetAttrRow().GetAttrByColumn(X, &CountOfAttr);

                k = 0;
                for (j = X; j < coordScreenBufferSize.X && it != itEnd; TargetPtr++, ++it)
                {
                    const WORD wLegacyAttributes = Attr.GetLegacyAttributes();
                    if ((j == X) && it->second.IsTrailing())
                    {
                        *TargetPtr = wLegacyAttributes;
                    }
                    else if (it->second.IsLeading())
                    {
                        if ((NumRead == *pcRecords - 1) || (j == coordScreenBufferSize.X - 1))
                        {
                            *TargetPtr = wLegacyAttributes;
                        }
                        else
                        {
                            *TargetPtr = wLegacyAttributes | it->second.GeneratePublicApiAttributeFormat();
                        }
                    }
                    else
                    {
                        *TargetPtr = wLegacyAttributes | it->second.GeneratePublicApiAttributeFormat();
                    }

                    NumRead++;
                    j += 1;

                    ++k;
                    if (static_cast<size_t>(k) == CountOfAttr && j < coordScreenBufferSize.X)
                    {
                        Attr = pRow->GetAttrRow().GetAttrByColumn(j, &CountOfAttr);
                        k = 0;
                    }

                    if (NumRead == *pcRecords)
                    {
                        delete[] TransBufferA;
                        return STATUS_SUCCESS;
                    }
                }

                try
                {
                    pRow = &screenInfo.GetTextBuffer().GetNextRowNoWrap(*pRow);
                }
                catch (...)
                {
                    return NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
                }

                X = 0;
                Y++;
                if (Y >= coordScreenBufferSize.Y)
                {
                    break;
                }
            }
        }
        else
        {
            *pcRecords = 0;
            delete[] TransBufferA;

            return STATUS_INVALID_PARAMETER;
        }
    }

    if (ulStringType == CONSOLE_ASCII)
    {
        UINT const Codepage = gci.OutputCP;

        NumRead = ConvertToOem(Codepage, TransBuffer, NumRead, (LPSTR)pvBuffer, *pcRecords);

        delete[] TransBuffer;
    }

    delete[] TransBufferA;

    *pcRecords = NumRead;
    return STATUS_SUCCESS;
}

void ScreenBufferSizeChange(const COORD coordNewSize)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    try
    {
        gci.pInputBuffer->Write(std::make_unique<WindowBufferSizeEvent>(coordNewSize));
    }
    catch (...)
    {
        LOG_HR(wil::ResultFromCaughtException());
    }
}

void ScrollScreen(SCREEN_INFORMATION& screenInfo,
                  const SMALL_RECT * const psrScroll,
                  _In_opt_ const SMALL_RECT * const psrMerge,
                  const COORD coordTarget)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (screenInfo.IsActiveScreenBuffer())
    {
        IAccessibilityNotifier *pNotifier = ServiceLocator::LocateAccessibilityNotifier();
        status = NT_TESTNULL(pNotifier);

        if (NT_SUCCESS(status))
        {
            pNotifier->NotifyConsoleUpdateScrollEvent(coordTarget.X - psrScroll->Left, coordTarget.Y - psrScroll->Right);
        }
        IRenderer* const pRender = ServiceLocator::LocateGlobals().pRender;
        if (pRender != nullptr)
        {
            // psrScroll is the source rectangle which gets written with the same dimensions to the coordTarget position.
            // Therefore the final rectangle starts with the top left corner at coordTarget
            // and the size is the size of psrScroll.
            // NOTE: psrScroll is an INCLUSIVE rectangle, so we must add 1 when measuring width as R-L or B-T
            Viewport scrollRect = Viewport::FromInclusive(*psrScroll);
            SMALL_RECT rcWritten = Viewport::FromDimensions(coordTarget, scrollRect.Width(), scrollRect.Height()).ToExclusive();

            pRender->TriggerRedraw(&rcWritten);

            // psrMerge was just filled exactly where it's stated.
            if (psrMerge != nullptr)
            {
                // psrMerge is an inclusive rectangle. Make it exclusive to deal with the renderer.
                SMALL_RECT rcMerge = Viewport::FromInclusive(*psrMerge).ToExclusive();

                pRender->TriggerRedraw(&rcMerge);
            }
        }
    }
}

// Routine Description:
// - This routine rotates the circular buffer as a shorthand for scrolling the entire screen
SHORT ScrollEntireScreen(SCREEN_INFORMATION& screenInfo, const SHORT sScrollValue)
{
    // store index of first row
    SHORT const RowIndex = screenInfo.GetTextBuffer().GetFirstRowIndex();

    // update screen buffer
    screenInfo.GetTextBuffer().SetFirstRowIndex((SHORT)((RowIndex + sScrollValue) % screenInfo.GetScreenBufferSize().Y));

    return RowIndex;
}

// Routine Description:
// - This routine is a special-purpose scroll for use by AdjustCursorPosition.
// Arguments:
// - screenInfo - reference to screen buffer info.
// Return Value:
// - true if we succeeded in scrolling the buffer, otherwise false (if we're out of memory)
bool StreamScrollRegion(SCREEN_INFORMATION& screenInfo)
{
    // Rotate the circular buffer around and wipe out the previous final line.
    bool fSuccess = screenInfo.GetTextBuffer().IncrementCircularBuffer();
    if (fSuccess)
    {
        // Trigger a graphical update if we're active.
        if (screenInfo.IsActiveScreenBuffer())
        {
            COORD coordDelta = { 0 };
            coordDelta.Y = -1;

            IAccessibilityNotifier *pNotifier = ServiceLocator::LocateAccessibilityNotifier();
            if (pNotifier)
            {
                // Notify accessibility that a scroll has occurred.
                pNotifier->NotifyConsoleUpdateScrollEvent(coordDelta.X, coordDelta.Y);
            }

            if (ServiceLocator::LocateGlobals().pRender != nullptr)
            {
                ServiceLocator::LocateGlobals().pRender->TriggerScroll(&coordDelta);
            }
        }
    }
    return fSuccess;
}

// Routine Description:
// - This routine copies ScrollRectangle to DestinationOrigin then fills in ScrollRectangle with Fill.
// - The scroll region is copied to a third buffer, the scroll region is filled, then the original contents of the scroll region are copied to the destination.
// Arguments:
// - screenInfo - reference to screen buffer info.
// - ScrollRectangle - Region to copy
// - ClipRectangle - Optional pointer to clip region.
// - DestinationOrigin - Upper left corner of target region.
// - Fill - Character and attribute to fill source region with.
// NOTE: Throws exceptions
void ScrollRegion(SCREEN_INFORMATION& screenInfo,
                  const SMALL_RECT scrollRectGiven,
                  const std::optional<SMALL_RECT> clipRectGiven,
                  const COORD destinationOriginGiven,
                  const CHAR_INFO fillGiven)
{
    auto fillWith = fillGiven;
    auto scrollRect = scrollRectGiven;
    auto originCoordinate = destinationOriginGiven;

    // here's how we clip:

    // Clip source rectangle to screen buffer => S
    // Create target rectangle based on S => T
    // Clip T to ClipRegion => T
    // Create S2 based on clipped T => S2
    // Clip S to ClipRegion => S3

    // S2 is the region we copy to T
    // S3 is the region to fill

    if (fillWith.Char.UnicodeChar == UNICODE_NULL && fillWith.Attributes == 0)
    {
        fillWith.Char.UnicodeChar = UNICODE_SPACE;
        fillWith.Attributes = screenInfo.GetAttributes().GetLegacyAttributes();
    }

    const auto bufferSize = screenInfo.GetScreenBufferSize();
    const COORD bufferLimits{ bufferSize.X - 1i16, bufferSize.Y - 1i16 };

    // clip the source rectangle to the screen buffer
    if (scrollRect.Left < 0)
    {
        originCoordinate.X += -scrollRect.Left;
        scrollRect.Left = 0;
    }
    if (scrollRect.Top < 0)
    {
        originCoordinate.Y += -scrollRect.Top;
        scrollRect.Top = 0;
    }

    if (scrollRect.Right >= bufferSize.X)
    {
        scrollRect.Right = bufferLimits.X;
    }
    if (scrollRect.Bottom >= bufferSize.Y)
    {
        scrollRect.Bottom = bufferLimits.Y;
    }

    // if source rectangle doesn't intersect screen buffer, return.
    if (scrollRect.Bottom < scrollRect.Top || scrollRect.Right < scrollRect.Left)
    {
        return;
    }

    // Account for the scroll margins set by DECSTBM
    auto marginRect = screenInfo.GetScrollMargins();
    const auto viewport = screenInfo.GetBufferViewport();

    // The margins are in viewport relative coordinates. Adjust for that.
    marginRect.Top += viewport.Top;
    marginRect.Bottom += viewport.Top;
    marginRect.Left += viewport.Left;
    marginRect.Right += viewport.Left;

    if (marginRect.Bottom > marginRect.Top)
    {
        if (scrollRect.Top < marginRect.Top)
        {
            scrollRect.Top = marginRect.Top;
        }
        if (scrollRect.Bottom >= marginRect.Bottom)
        {
            scrollRect.Bottom = marginRect.Bottom;
        }
    }

    // clip the target rectangle
    // if a cliprectangle was provided, clip it to the screen buffer.
    // if not, set the cliprectangle to the screen buffer region.
    auto clipRect = clipRectGiven.value_or(SMALL_RECT{ 0, 0, bufferSize.X - 1i16, bufferSize.Y - 1i16 });

    // clip the cliprectangle.
    clipRect.Left = std::max(clipRect.Left, 0i16);
    clipRect.Top = std::max(clipRect.Top, 0i16);
    clipRect.Right = std::min(clipRect.Right, bufferLimits.X);
    clipRect.Bottom = std::min(clipRect.Bottom, bufferLimits.Y);

    // Account for the scroll margins set by DECSTBM
    if (marginRect.Bottom > marginRect.Top)
    {
        if (clipRect.Top < marginRect.Top)
        {
            clipRect.Top = marginRect.Top;
        }
        if (clipRect.Bottom >= marginRect.Bottom)
        {
            clipRect.Bottom = marginRect.Bottom;
        }
    }
    // Create target rectangle based on S => T
    // Clip T to ClipRegion => T
    // Create S2 based on clipped T => S2

    auto scrollRect2 = scrollRect;

    SMALL_RECT targetRectangle;
    targetRectangle.Left = originCoordinate.X;
    targetRectangle.Top = originCoordinate.Y;
    targetRectangle.Right = (SHORT)(originCoordinate.X + (scrollRect2.Right - scrollRect2.Left + 1) - 1);
    targetRectangle.Bottom = (SHORT)(originCoordinate.Y + (scrollRect2.Bottom - scrollRect2.Top + 1) - 1);

    if (targetRectangle.Left < clipRect.Left)
    {
        scrollRect2.Left += clipRect.Left - targetRectangle.Left;
        targetRectangle.Left = clipRect.Left;
    }
    if (targetRectangle.Top < clipRect.Top)
    {
        scrollRect2.Top += clipRect.Top - targetRectangle.Top;
        targetRectangle.Top = clipRect.Top;
    }
    if (targetRectangle.Right > clipRect.Right)
    {
        scrollRect2.Right -= targetRectangle.Right - clipRect.Right;
        targetRectangle.Right = clipRect.Right;
    }
    if (targetRectangle.Bottom > clipRect.Bottom)
    {
        scrollRect2.Bottom -= targetRectangle.Bottom - clipRect.Bottom;
        targetRectangle.Bottom = clipRect.Bottom;
    }

    // clip scroll rect to clipregion => S3
    SMALL_RECT scrollRect3 = scrollRect;
    scrollRect3.Left = std::max(scrollRect3.Left, clipRect.Left);
    scrollRect3.Top = std::max(scrollRect3.Top, clipRect.Top);
    scrollRect3.Right = std::min(scrollRect3.Right, clipRect.Right);
    scrollRect3.Bottom = std::min(scrollRect3.Bottom, clipRect.Bottom);

    // if scroll rect doesn't intersect clip region, return.
    if (scrollRect3.Bottom < scrollRect3.Top || scrollRect3.Right < scrollRect3.Left)
    {
        return;
    }

    // if target rectangle doesn't intersect screen buffer, skip scrolling part.
    if (!(targetRectangle.Bottom < targetRectangle.Top || targetRectangle.Right < targetRectangle.Left))
    {
        // if we can, don't use intermediate scroll region buffer.  do this
        // by figuring out fill rectangle.  NOTE: this code will only work
        // if _CopyRectangle copies from low memory to high memory (otherwise
        // we would overwrite the scroll region before reading it).

        if (scrollRect2.Right == targetRectangle.Right &&
            scrollRect2.Left == targetRectangle.Left && scrollRect2.Top > targetRectangle.Top && scrollRect2.Top < targetRectangle.Bottom)
        {
            const COORD targetPoint{ targetRectangle.Left, targetRectangle.Top };

            if (scrollRect2.Right == (SHORT)(bufferSize.X - 1) &&
                scrollRect2.Left == 0 && scrollRect2.Bottom == (SHORT)(bufferSize.Y - 1) && scrollRect2.Top == 1)
            {
                ScrollEntireScreen(screenInfo, (SHORT)(scrollRect2.Top - targetRectangle.Top));
            }
            else
            {
                _CopyRectangle(screenInfo, scrollRect2, targetPoint);
            }

            SMALL_RECT fillRect;
            fillRect.Left = targetRectangle.Left;
            fillRect.Right = targetRectangle.Right;
            fillRect.Top = (SHORT)(targetRectangle.Bottom + 1);
            fillRect.Bottom = scrollRect.Bottom;
            if (fillRect.Top < clipRect.Top)
            {
                fillRect.Top = clipRect.Top;
            }

            if (fillRect.Bottom > clipRect.Bottom)
            {
                fillRect.Bottom = clipRect.Bottom;
            }

            FillRectangle(&fillWith, screenInfo, &fillRect);

            ScrollScreen(screenInfo, &scrollRect2, &fillRect, targetPoint);
        }

        // if no overlap, don't need intermediate copy
        else if (scrollRect3.Right < targetRectangle.Left ||
                 scrollRect3.Left > targetRectangle.Right ||
                 scrollRect3.Top > targetRectangle.Bottom ||
                 scrollRect3.Bottom < targetRectangle.Top)
        {
            const COORD TargetPoint{ targetRectangle.Left, targetRectangle.Top };
            _CopyRectangle(screenInfo, scrollRect2, TargetPoint);
            FillRectangle(&fillWith, screenInfo, &scrollRect3);
            ScrollScreen(screenInfo, &scrollRect2, &scrollRect3, TargetPoint);
        }

        // for the case where the source and target rectangles overlap, we copy the source rectangle, fill it, then copy it to the target.
        else
        {
            const COORD size{ scrollRect2.Right - scrollRect2.Left + 1i16,
                              scrollRect2.Bottom - scrollRect2.Top + 1i16 };
            const SMALL_RECT targetRect{ 0, 0, scrollRect2.Right - scrollRect2.Left, scrollRect2.Bottom - scrollRect2.Top };
            const COORD sourcePoint{ scrollRect2.Left, scrollRect2.Top };

            std::vector<std::vector<OutputCell>> outputCells;
            outputCells = ReadRectFromScreenBuffer(screenInfo, sourcePoint, Viewport::FromInclusive(targetRect));

            FillRectangle(&fillWith, screenInfo, &scrollRect3);

            const SMALL_RECT sourceRect{ 0, 0, size.X - 1i16, size.Y - 1i16 };
            const COORD targetPoint{ targetRectangle.Left, targetRectangle.Top };

            WriteRectToScreenBuffer(screenInfo, outputCells, targetPoint);

            // update regions on screen.
            ScrollScreen(screenInfo, &scrollRect2, &scrollRect3, targetPoint);
        }
    }
    else
    {
        // Do fill.
        FillRectangle(&fillWith, screenInfo, &scrollRect3);

        WriteToScreen(screenInfo, scrollRect3);
    }
}

void SetActiveScreenBuffer(SCREEN_INFORMATION& screenInfo)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.pCurrentScreenBuffer = &screenInfo;

    // initialize cursor
    screenInfo.GetTextBuffer().GetCursor().SetIsOn(false);

    // set font
    screenInfo.RefreshFontWithRenderer();

    // Empty input buffer.
    gci.pInputBuffer->FlushAllButKeys();
    SetScreenColors(screenInfo,
                    screenInfo.GetAttributes().GetLegacyAttributes(),
                    screenInfo.GetPopupAttributes()->GetLegacyAttributes(),
                    FALSE);

    // Set window size.
    screenInfo.PostUpdateWindowSize();

    gci.ConsoleIme.RefreshAreaAttributes();

    // Write data to screen.
    WriteToScreen(screenInfo, screenInfo.GetBufferViewport());
}

// TODO: MSFT 9450717 This should join the ProcessList class when CtrlEvents become moved into the server. https://osgvsowi/9450717
void CloseConsoleProcessState()
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    // If there are no connected processes, sending control events is pointless as there's no one do send them to. In
    // this case we'll just exit conhost.

    // N.B. We can get into this state when a process has a reference to the console but hasn't connected. For example,
    //      when it's created suspended and never resumed.
    if (gci.ProcessHandleList.IsEmpty())
    {
        ServiceLocator::RundownAndExit(STATUS_SUCCESS);
    }

    HandleCtrlEvent(CTRL_CLOSE_EVENT);
}
