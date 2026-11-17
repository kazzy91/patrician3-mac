// window.m - the Mac window: shows the game's picture with Metal and feeds mouse and keyboard to input.c.
//
// The game runs on the main thread and never returns to a Cocoa run loop, so events are taken by hand in
// window_pump() (called from input_pump(), i.e. from the game's own PeekMessage / GetMessage / Sleep calls).
// The picture is the DirectDraw primary surface, uploaded as a texture and drawn scaled to the window with its aspect
// ratio kept (black bars otherwise). The window opens at the first presented frame.
//
//   P3_HEADLESS=1          no window (tests; use P3_FRAMES to see the picture)
//   P3_WINDOW_DUMP=f.png   write what Metal actually drew (read back from the drawable) once a second: for checks
//   P3_NO_ACTIVATE=1       don't bring the window to the front
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "host.h"

void input_mouse_move(int x, int y);
void input_mouse_button(int button, int down);
void input_mouse_wheel(int delta);
void input_key(int vk, int down, int ch);
void input_request_close(void);
int user32_cursor_hidden(void);
int png_write(const char *path, const uint8_t *pixels, int w, int h, int pitch);

@interface GameView : NSView
@end
@implementation GameView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
@end

@interface GameDelegate : NSObject <NSWindowDelegate>
@end
@implementation GameDelegate
- (BOOL)windowShouldClose:(NSWindow *)w { input_request_close(); return NO; }
- (void)quit:(id)sender { input_request_close(); }
- (void)debugDump:(id)sender { void ddraw_debug_dump(void); ddraw_debug_dump(); }
@end

static int enabled;
static NSWindow *win;
static GameView *view;
static GameDelegate *delegate;
static CAMetalLayer *layer;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;
static id<MTLSamplerState> sampler;
static id<MTLTexture> texture;
static int tex_w, tex_h;
static int cursor_hidden;
static int button_down[3];

int window_active(void) { return enabled; }

void window_init(void) {
    if (getenv("P3_HEADLESS")) return;
    enabled = 1;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [GameDelegate new];
        NSMenu *bar = [NSMenu new], *app = [NSMenu new], *window = [[NSMenu alloc] initWithTitle:@"Window"];
        NSMenuItem *appItem = [NSMenuItem new], *windowItem = [NSMenuItem new];
        [bar addItem:appItem];
        [bar addItem:windowItem];
        [appItem setSubmenu:app];
        [windowItem setSubmenu:window];
        [app addItemWithTitle:@"Hide Patrician III" action:@selector(hide:) keyEquivalent:@"h"];
        [app addItem:[NSMenuItem separatorItem]];
        NSMenuItem *dump = [app addItemWithTitle:@"Save Debug Dump" action:@selector(debugDump:) keyEquivalent:@"D"];
        dump.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
        dump.target = delegate;
        NSMenuItem *quit = [app addItemWithTitle:@"Quit Patrician III" action:@selector(quit:) keyEquivalent:@"q"];
        quit.target = delegate;
        NSMenuItem *fs = [window addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
        [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [NSApp setMainMenu:bar];
        [NSApp finishLaunching];
    }
}

static NSString *const shader_source = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 *rect [[buffer(0)]]) {\n"
    "    float2 p = float2(vid & 1, vid >> 1);\n"
    "    V o;\n"
    "    o.pos = float4(mix(rect[0].xy, rect[0].zw, p), 0, 1);\n"
    "    o.uv = float2(p.x, 1 - p.y);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "    return float4(t.sample(s, in.uv).rgb, 1);\n"
    "}\n";

static void create_window(int w, int h) {
    device = MTLCreateSystemDefaultDevice();
    if (!device) die("no Metal device");
    queue = [device newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:shader_source options:nil error:&err];
    if (!lib) die("Metal shader: %s", err.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pipeline) die("Metal pipeline: %s", err.localizedDescription.UTF8String);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sampler = [device newSamplerStateWithDescriptor:sd];

    NSRect vf = [NSScreen mainScreen].visibleFrame;
    double sc = MIN(vf.size.width * 0.92 / w, (vf.size.height - 28) * 0.92 / h);
    NSRect content = NSMakeRect(0, 0, floor(w * sc), floor(h * sc));
    win = [[NSWindow alloc] initWithContentRect:content
                                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                        backing:NSBackingStoreBuffered defer:NO];
    win.title = @"Patrician III";
    win.delegate = delegate;
    win.releasedWhenClosed = NO;
    win.backgroundColor = NSColor.blackColor;
    win.acceptsMouseMovedEvents = YES;
    win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    view = [[GameView alloc] initWithFrame:content];
    view.wantsLayer = YES;
    win.contentView = view;
    layer = (CAMetalLayer *)view.layer;
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = NO;                         // the game paces itself (Sleep, WaitForVerticalBlank)
    layer.framebufferOnly = getenv("P3_WINDOW_DUMP") == NULL;
    [win center];
    [win makeFirstResponder:view];
    if (getenv("P3_NO_ACTIVATE")) [win orderFront:nil];
    else { [win makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
    printf("boot: window %dx%d points for a %dx%d picture\n", (int)content.size.width, (int)content.size.height, w, h);
    fflush(stdout);
}

void window_present(const uint32_t *xrgb, int w, int h) {
    if (!enabled) return;
    @autoreleasepool {
        if (!win) create_window(w, h);
        if (!texture || tex_w != w || tex_h != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
            texture = [device newTextureWithDescriptor:td];
            tex_w = w; tex_h = h;
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:xrgb bytesPerRow:4 * w];
        CGSize ds = [view convertSizeToBacking:view.bounds.size];
        if (ds.width < 1 || ds.height < 1) return;
        if (!CGSizeEqualToSize(layer.drawableSize, ds)) layer.drawableSize = ds;
        id<CAMetalDrawable> d = [layer nextDrawable];
        if (!d) return;
        double da = ds.width / ds.height, ga = (double)w / h;
        float sx = da > ga ? (float)(ga / da) : 1, sy = da > ga ? 1 : (float)(da / ga);
        float rect[4] = {-sx, -sy, sx, sy};
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pipeline];
        [enc setVertexBytes:rect length:sizeof rect atIndex:0];
        [enc setFragmentTexture:texture atIndex:0];
        [enc setFragmentSamplerState:sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        static uint64_t last_dump;
        const char *dump = getenv("P3_WINDOW_DUMP");
        if (dump && host_ticks() - last_dump > 1000000000ull) {
            last_dump = host_ticks();
            [cb commit];
            [cb waitUntilCompleted];
            int dw = (int)d.texture.width, dh = (int)d.texture.height;
            uint8_t *buf = malloc((size_t)4 * dw * dh);
            [d.texture getBytes:buf bytesPerRow:4 * dw fromRegion:MTLRegionMake2D(0, 0, dw, dh) mipmapLevel:0];
            png_write(dump, buf, dw, dh, 4 * dw);
            free(buf);
            printf("boot: window drawable %dx%d -> %s\n", dw, dh, dump);
            fflush(stdout);
            [d present];
        } else {
            [cb presentDrawable:d];
            [cb commit];
        }
    }
}

// ---------------- events ----------------
// macOS virtual key code -> Windows virtual key.
static const uint8_t vk_of[128] = {
    [0x00] = 'A', [0x01] = 'S', [0x02] = 'D', [0x03] = 'F', [0x04] = 'H', [0x05] = 'G', [0x06] = 'Z', [0x07] = 'X',
    [0x08] = 'C', [0x09] = 'V', [0x0B] = 'B', [0x0C] = 'Q', [0x0D] = 'W', [0x0E] = 'E', [0x0F] = 'R', [0x10] = 'Y',
    [0x11] = 'T', [0x12] = '1', [0x13] = '2', [0x14] = '3', [0x15] = '4', [0x16] = '6', [0x17] = '5', [0x18] = 0xBB,
    [0x19] = '9', [0x1A] = '7', [0x1B] = 0xBD, [0x1C] = '8', [0x1D] = '0', [0x1E] = 0xDD, [0x1F] = 'O', [0x20] = 'U',
    [0x21] = 0xDB, [0x22] = 'I', [0x23] = 'P', [0x24] = 0x0D, [0x25] = 'L', [0x26] = 'J', [0x27] = 0xDE, [0x28] = 'K',
    [0x29] = 0xBA, [0x2A] = 0xDC, [0x2B] = 0xBC, [0x2C] = 0xBF, [0x2D] = 'N', [0x2E] = 'M', [0x2F] = 0xBE,
    [0x30] = 0x09, [0x31] = 0x20, [0x32] = 0xC0, [0x33] = 0x08, [0x35] = 0x1B,
    [0x38] = 0x10, [0x39] = 0x14, [0x3A] = 0x12, [0x3B] = 0x11, [0x3C] = 0x10, [0x3D] = 0x12, [0x3E] = 0x11,
    [0x41] = 0x6E, [0x43] = 0x6A, [0x45] = 0x6B, [0x4B] = 0x6F, [0x4C] = 0x0D, [0x4E] = 0x6D,
    [0x52] = 0x60, [0x53] = 0x61, [0x54] = 0x62, [0x55] = 0x63, [0x56] = 0x64, [0x57] = 0x65, [0x58] = 0x66, [0x59] = 0x67,
    [0x5B] = 0x68, [0x5C] = 0x69,
    [0x60] = 0x74, [0x61] = 0x75, [0x62] = 0x76, [0x63] = 0x72, [0x64] = 0x77, [0x65] = 0x78, [0x67] = 0x7A, [0x6D] = 0x79,
    [0x6F] = 0x7B, [0x73] = 0x24, [0x74] = 0x21, [0x75] = 0x2E, [0x76] = 0x73, [0x77] = 0x23, [0x78] = 0x71, [0x79] = 0x22,
    [0x7A] = 0x70, [0x7B] = 0x25, [0x7C] = 0x27, [0x7D] = 0x28, [0x7E] = 0x26,
};

// The event's position in game pixels; returns whether it is inside the window's content.
static BOOL game_point(NSEvent *e, int *gx, int *gy) {
    if (!win || !tex_w || e.window != win) return NO;
    NSPoint p = [view convertPoint:e.locationInWindow fromView:nil];
    NSRect b = view.bounds;
    double sc = MIN(b.size.width / tex_w, b.size.height / tex_h);
    double ox = (b.size.width - tex_w * sc) / 2, oy = (b.size.height - tex_h * sc) / 2;
    *gx = (int)floor((p.x - ox) / sc);
    *gy = (int)floor((b.size.height - p.y - oy) / sc);
    return NSPointInRect(p, b);
}
static void set_cursor_hidden(int hide) {
    if (hide == cursor_hidden) return;
    cursor_hidden = hide;
    if (hide) [NSCursor hide]; else [NSCursor unhide];
}

// Returns YES when the event is consumed (not passed on to Cocoa).
static BOOL handle(NSEvent *e) {
    int x, y;
    switch (e.type) {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp: {
        if (e.modifierFlags & NSEventModifierFlagCommand) return NO;     // menu shortcuts
        if (!win || !win.isKeyWindow) return NO;
        if (e.keyCode == 0x6F) {                           // F12: debug dump (not passed to the game)
            void ddraw_debug_dump(void);
            if (e.type == NSEventTypeKeyDown && !e.isARepeat) ddraw_debug_dump();
            return YES;
        }
        int vk = e.keyCode < 128 ? vk_of[e.keyCode] : 0, ch = 0;
        if (e.type == NSEventTypeKeyDown && e.characters.length == 1) {
            unichar u = [e.characters characterAtIndex:0];
            ch = u == 0x7F ? 8 : u < 256 && (u >= 0x20 || u == '\r' || u == '\t' || u == 0x1B || u == 8) ? u : 0;
        }
        input_key(vk, e.type == NSEventTypeKeyDown, ch);
        return YES;
    }
    case NSEventTypeFlagsChanged: {
        static NSEventModifierFlags last;
        NSEventModifierFlags f = e.modifierFlags, changed = f ^ last;
        last = f;
        if (changed & NSEventModifierFlagShift) input_key(0x10, (f & NSEventModifierFlagShift) != 0, 0);
        if (changed & NSEventModifierFlagControl) input_key(0x11, (f & NSEventModifierFlagControl) != 0, 0);
        if (changed & NSEventModifierFlagOption) input_key(0x12, (f & NSEventModifierFlagOption) != 0, 0);
        return NO;
    }
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: {
        BOOL inside = game_point(e, &x, &y);
        if (inside || button_down[0] || button_down[1] || button_down[2]) input_mouse_move(x, y);
        set_cursor_hidden(inside && win.isKeyWindow && user32_cursor_hidden());
        return NO;
    }
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown: {
        int b = e.type == NSEventTypeLeftMouseDown ? 0 : e.type == NSEventTypeRightMouseDown ? 1 : 2;
        if (game_point(e, &x, &y)) {
            input_mouse_move(x, y);
            input_mouse_button(b, 1);
            button_down[b] = 1;
        }
        return NO;
    }
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp: {
        int b = e.type == NSEventTypeLeftMouseUp ? 0 : e.type == NSEventTypeRightMouseUp ? 1 : 2;
        if (button_down[b]) {
            if (game_point(e, &x, &y) || win) input_mouse_move(x, y);
            input_mouse_button(b, 0);
            button_down[b] = 0;
        }
        return NO;
    }
    case NSEventTypeScrollWheel: {
        static double acc;
        if (!game_point(e, &x, &y)) return NO;
        acc += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
        while (acc >= 1) { input_mouse_wheel(120); acc -= 1; }
        while (acc <= -1) { input_mouse_wheel(-120); acc += 1; }
        return YES;
    }
    default:
        return NO;
    }
}

void window_pump(void) {
    if (!enabled) return;
    @autoreleasepool {
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            if (!handle(e)) [NSApp sendEvent:e];
        }
        if (win && !win.isKeyWindow) set_cursor_hidden(0);
    }
    void ddraw_flush_present(void);
    ddraw_flush_present();                 // a frame another thread presented since the last pump
}
