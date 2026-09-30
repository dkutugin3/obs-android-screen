#include "placeholder.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

namespace placeholder {

static CFStringRef makeCFString(const std::string &s)
{
	return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

static void drawLine(CGContextRef ctx, const std::string &text, const std::string &fontName, CGFloat fontSize,
	CGFloat x, CGFloat y)
{
	if (text.empty())
		return;
	CFStringRef cfText = makeCFString(text);
	CFStringRef cfFont = makeCFString(fontName);

	CTFontRef font = CTFontCreateWithName(cfFont, fontSize, nullptr);
	CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
	CFTypeRef values[] = {font, CGColorGetConstantColor(kCGColorWhite)};
	CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, (const void **)keys, (const void **)values,
		2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CFAttributedStringRef attrString =
		CFAttributedStringCreate(kCFAllocatorDefault, cfText, attrs);
	CTLineRef line = CTLineCreateWithAttributedString(attrString);

	CGContextSetTextPosition(ctx, x, y);
	CTLineDraw(line, ctx);

	CFRelease(line);
	CFRelease(attrString);
	CFRelease(attrs);
	CFRelease(font);
	CFRelease(cfFont);
	CFRelease(cfText);
}

Frame render(const std::string &line1, const std::string &line2, uint32_t width, uint32_t height)
{
	Frame f;
	if (width < 2 || height < 2)
		return f;
	width &= ~1u;
	height &= ~1u;

	size_t ySize = (size_t)width * height;
	size_t uvSize = ySize / 2;
	f.width = width;
	f.height = height;
	f.data.assign(ySize + uvSize, 0);

	// Dark background, neutral chroma
	memset(f.data.data(), 0x10, ySize);
	memset(f.data.data() + ySize, 0x80, uvSize);

	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	CGContextRef ctx = CGBitmapContextCreate(f.data.data(), width, height, 8, width, gray,
		kCGImageAlphaNone | kCGBitmapByteOrderDefault);
	if (!ctx) {
		CGColorSpaceRelease(gray);
		return f;
	}

	CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorBlack));
	CGContextFillRect(ctx, CGRectMake(0, 0, width, height));

	CGFloat margin = (CGFloat)width * 0.08;
	CGFloat fontSize1 = (CGFloat)height / 14.0f;
	CGFloat fontSize2 = (CGFloat)height / 24.0f;

	// CoreText in a CGBitmap context renders upright glyphs as-is with a
	// bottom-left origin (y up). Do NOT flip the CTM: the bitmap device space
	// already matches the buffer top-down, and a flip mirrors the glyphs
	// (verified empirically — bug #7). Positions are converted "from the top".
	CGContextSetShouldAntialias(ctx, true);
	CGContextSetShouldSmoothFonts(ctx, false);

	CGFloat base1 = (CGFloat)height - margin - (CGFloat)height * 0.32f;
	drawLine(ctx, line1, "Helvetica-Bold", fontSize1, margin, base1);
	drawLine(ctx, line2, "Helvetica", fontSize2, margin, base1 - fontSize1 * 1.5f);

	CGContextRelease(ctx);
	CGColorSpaceRelease(gray);
	return f;
}

} // namespace placeholder
