/*
 * mkicon - 生成 iOS App 图标
 *
 *   mkicon <out.png> <size>              内置画一个(绿提示符)
 *   mkicon <out.png> <size> <src.png>    用现成的图(自动裁掉透明边再铺满整格)
 *
 * 只用 CoreGraphics/ImageIO，纯 macOS 侧工具，不参与 iOS 构建。
 */
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 内置图案 ---------------- */

static void addRoundRect(CGContextRef ctx, CGRect r, CGFloat radius)
{
    CGPathRef p = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
    CGContextAddPath(ctx, p);
    CGPathRelease(p);
}

static void drawBuiltin(CGContextRef ctx, CGFloat S)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[8] = { 0.145, 0.169, 0.184, 1.0,   0.039, 0.051, 0.059, 1.0 };
    CGFloat locs[2] = { 0.0, 1.0 };
    CGGradientRef grad = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(0, S), CGPointMake(0, 0), 0);
    CGGradientRelease(grad);
    CGColorSpaceRelease(cs);

    CGColorRef edge = CGColorCreateGenericRGB(1.0, 1.0, 1.0, 0.10);
    CGContextSetStrokeColorWithColor(ctx, edge);
    CGColorRelease(edge);
    CGFloat lw = S * 0.012;
    CGContextSetLineWidth(ctx, lw < 1.0 ? 1.0 : lw);
    addRoundRect(ctx, CGRectMake(S * 0.02, S * 0.02, S * 0.96, S * 0.96), S * 0.14);
    CGContextStrokePath(ctx);

    CGColorRef green = CGColorCreateGenericRGB(0.29, 0.85, 0.44, 1.0);
    CGContextSetStrokeColorWithColor(ctx, green);
    CGContextSetLineWidth(ctx, S * 0.085);
    CGContextSetLineCap(ctx, kCGLineCapRound);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);
    CGContextBeginPath(ctx);
    CGContextMoveToPoint(ctx, S * 0.27, S * 0.31);
    CGContextAddLineToPoint(ctx, S * 0.45, S * 0.50);
    CGContextAddLineToPoint(ctx, S * 0.27, S * 0.69);
    CGContextStrokePath(ctx);

    CGContextSetFillColorWithColor(ctx, green);
    addRoundRect(ctx, CGRectMake(S * 0.55, S * 0.445, S * 0.20, S * 0.11), S * 0.022);
    CGContextFillPath(ctx);
    CGColorRelease(green);
}

/* ---------------- 读图 / 裁透明边 ---------------- */

static CGImageRef loadImage(const char *path)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), false);
    if (!url) return NULL;
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    return img;
}

static int alphaBox(CGImageRef img, size_t *bx0, size_t *by0, size_t *bx1, size_t *by1)
{
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (!w || !h || w > 8192 || h > 8192) return 0;
    UInt8 *buf = (UInt8 *)calloc(w * h * 4, 1);
    if (!buf) return 0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!c) { free(buf); return 0; }
    CGContextDrawImage(c, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
    CGContextRelease(c);

    size_t minx = w, miny = h, maxx = 0, maxy = 0;
    int any = 0;
    for (size_t y = 0; y < h; y++) {
        const UInt8 *row = buf + y * w * 4;
        for (size_t x = 0; x < w; x++) {
            if (row[x * 4 + 3] > 16) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                any = 1;
            }
        }
    }
    free(buf);
    if (!any) return 0;
    /* bitmap 的第 0 行就是图的最上面一行，所以这里和 CGImageCreateWithImageInRect 的坐标一致 */
    *bx0 = minx; *by0 = miny; *bx1 = maxx + 1; *by1 = maxy + 1;
    return 1;
}

static void drawSource(CGContextRef ctx, CGImageRef src, CGFloat S)
{
    size_t x0 = 0, y0 = 0, x1 = CGImageGetWidth(src), y1 = CGImageGetHeight(src);
    if (alphaBox(src, &x0, &y0, &x1, &y1)) {
        fprintf(stderr, "  裁到 %zux%zu (原图 %zux%zu)\n",
                x1 - x0, y1 - y0, CGImageGetWidth(src), CGImageGetHeight(src));
    } else {
        x0 = 0; y0 = 0; x1 = CGImageGetWidth(src); y1 = CGImageGetHeight(src);
    }
    CGImageRef sub = CGImageCreateWithImageInRect(src, CGRectMake((CGFloat)x0, (CGFloat)y0,
                                                                  (CGFloat)(x1 - x0), (CGFloat)(y1 - y0)));
    if (!sub) return;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    /* 铺满整格：iOS 自己会再切圆角 */
    CGContextDrawImage(ctx, CGRectMake(0, 0, S, S), sub);
    CGImageRelease(sub);
}

/* ---------------- 输出 ---------------- */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.png> <size> [src.png]\n", argv[0]);
        return 2;
    }
    int size = atoi(argv[2]);
    if (size < 16 || size > 4096) { fprintf(stderr, "bad size %d\n", size); return 2; }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, (size_t)size, (size_t)size, 8, 0, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return 1;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextSetAllowsAntialiasing(ctx, true);

    if (argc >= 4) {
        CGImageRef src = loadImage(argv[3]);
        if (!src) { fprintf(stderr, "读不了源图: %s\n", argv[3]); CGContextRelease(ctx); return 1; }
        drawSource(ctx, src, (CGFloat)size);
        CGImageRelease(src);
    } else {
        drawBuiltin(ctx, (CGFloat)size);
    }

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return 1;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1],
                                                           (CFIndex)strlen(argv[1]), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CFRelease(url);
    if (!dst) { CGImageRelease(img); return 1; }
    CGImageDestinationAddImage(dst, img, NULL);
    int ok = CGImageDestinationFinalize(dst) ? 0 : 1;
    CFRelease(dst);
    CGImageRelease(img);
    if (ok) fprintf(stderr, "写不了: %s\n", argv[1]);
    return ok;
}
