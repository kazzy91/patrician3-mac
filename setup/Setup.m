// Setup.m - "Patrician III Setup.app": asks for the user's Patrician III folder, runs install.sh (which translates and
// compiles the game on this Mac) and shows its progress. The app it builds lands in Applications.
// Built by scripts/make_dmg.sh.
#import <Cocoa/Cocoa.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
static const int STEPS = 6;

@interface Setup : NSObject <NSApplicationDelegate>
@end

@implementation Setup {
    NSWindow *window;
    NSTextField *pathField, *status;
    NSButton *chooseButton, *buildButton;
    NSProgressIndicator *progress;
    NSTextView *logView;
    NSString *gamePath, *installedApp, *lastError;
    NSFileHandle *logFile;
    NSMutableData *pending;
    pid_t child;
    id activity;
}

static NSTextField *label(NSString *text, CGFloat size, BOOL bold) {
    NSTextField *t = [NSTextField wrappingLabelWithString:text];
    t.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    return t;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    [self buildMenu];
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 520)
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                           backing:NSBackingStoreBuffered defer:NO];
    window.title = @"Patrician III Setup";
    NSView *v = window.contentView;

    NSTextField *title = label(@"Patrician III for Mac", 22, YES);
    NSTextField *intro = label(@"Setup builds the game on this Mac from your own copy of Patrician III "
                               @"(English, retail CD) and puts it in Applications. Choose the folder the game is "
                               @"installed in, for example inside a CrossOver bottle.", 13, NO);
    intro.preferredMaxLayoutWidth = 600;
    pathField = label(@"No folder chosen", 12, NO);
    pathField.textColor = NSColor.secondaryLabelColor;
    pathField.lineBreakMode = NSLineBreakByTruncatingMiddle;
    pathField.maximumNumberOfLines = 1;
    chooseButton = [NSButton buttonWithTitle:@"Choose Folder…" target:self action:@selector(choose:)];
    buildButton = [NSButton buttonWithTitle:@"Build and Install" target:self action:@selector(build:)];
    buildButton.keyEquivalent = @"\r";
    buildButton.enabled = NO;
    progress = [NSProgressIndicator new];
    progress.indeterminate = NO;
    progress.minValue = 0;
    progress.maxValue = STEPS;
    status = label(@"", 12, NO);
    status.preferredMaxLayoutWidth = 600;

    NSScrollView *scroll = [NSTextView scrollableTextView];
    logView = scroll.documentView;
    logView.editable = NO;
    logView.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    logView.textColor = NSColor.secondaryLabelColor;
    scroll.borderType = NSBezelBorder;

    for (NSView *x in @[title, intro, pathField, chooseButton, buildButton, progress, status, scroll]) {
        x.translatesAutoresizingMaskIntoConstraints = NO;
        [v addSubview:x];
    }
    NSDictionary *d = NSDictionaryOfVariableBindings(title, intro, pathField, chooseButton, buildButton, progress, status, scroll);
    for (NSString *f in @[@"H:|-20-[title]-20-|", @"H:|-20-[intro]-20-|", @"H:|-20-[pathField]-[chooseButton]-20-|",
                          @"H:|-20-[progress]-20-|", @"H:|-20-[status]-[buildButton]-20-|", @"H:|-20-[scroll]-20-|",
                          @"V:|-18-[title]-10-[intro]-16-[chooseButton]-16-[progress]-10-[buildButton]-14-[scroll(>=160)]-20-|"])
        [v addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:f options:0 metrics:nil views:d]];
    [v addConstraint:[NSLayoutConstraint constraintWithItem:pathField attribute:NSLayoutAttributeCenterY relatedBy:NSLayoutRelationEqual
                                                     toItem:chooseButton attribute:NSLayoutAttributeCenterY multiplier:1 constant:0]];
    [v addConstraint:[NSLayoutConstraint constraintWithItem:status attribute:NSLayoutAttributeCenterY relatedBy:NSLayoutRelationEqual
                                                     toItem:buildButton attribute:NSLayoutAttributeCenterY multiplier:1 constant:0]];
    [pathField setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    NSString *found = [self findGame];
    if (found) [self setGame:found note:@"Found in CrossOver. Choose another folder if this isn't the one."];
    [self checkTools];
}

- (void)buildMenu {
    NSMenu *bar = [NSMenu new], *app = [NSMenu new], *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [app addItemWithTitle:@"Hide Patrician III Setup" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quit Patrician III Setup" action:@selector(terminate:) keyEquivalent:@"q"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    NSMenuItem *a = [NSMenuItem new], *e = [NSMenuItem new];
    a.submenu = app;
    e.submenu = edit;
    [bar addItem:a];
    [bar addItem:e];
    NSApp.mainMenu = bar;
}

// The usual CrossOver location: ~/Library/Application Support/CrossOver/Bottles/<bottle>/drive_c/Program Files*/<game>.
- (NSString *)findGame {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *bottles = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CrossOver/Bottles"];
    for (NSString *b in [fm contentsOfDirectoryAtPath:bottles error:nil])
        for (NSString *pf in @[@"Program Files (x86)", @"Program Files"]) {
            NSString *dir = [NSString pathWithComponents:@[bottles, b, @"drive_c", pf]];
            for (NSString *g in [fm contentsOfDirectoryAtPath:dir error:nil]) {
                NSString *p = [dir stringByAppendingPathComponent:g];
                if ([self hasGame:p]) return p;
            }
        }
    return nil;
}

- (BOOL)hasGame:(NSString *)dir {
    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil])
        if ([f caseInsensitiveCompare:@"Patrician3.exe"] == NSOrderedSame) return YES;
    return NO;
}

- (void)setGame:(NSString *)path note:(NSString *)note {
    gamePath = path;
    pathField.stringValue = [path stringByAbbreviatingWithTildeInPath];
    pathField.toolTip = path;
    pathField.textColor = NSColor.labelColor;
    buildButton.enabled = YES;
    [self setStatus:note error:NO];
}

- (void)setStatus:(NSString *)text error:(BOOL)error {
    status.stringValue = text ?: @"";
    status.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

- (void)checkTools {
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcode-select"];
    t.arguments = @[@"-p"];
    t.standardOutput = t.standardError = NSFileHandle.fileHandleWithNullDevice;
    if ([t launchAndReturnError:nil]) [t waitUntilExit];
    if (t.terminationStatus == 0) return;
    NSAlert *a = [NSAlert new];
    a.messageText = @"Setup needs Apple's Command Line Tools";
    a.informativeText = @"They're free and include the compiler Setup uses to build the game. Click Install, wait "
                        @"until the installer finishes, then click Build and Install.";
    [a addButtonWithTitle:@"Install"];
    [a addButtonWithTitle:@"Not Now"];
    if ([a runModal] == NSAlertFirstButtonReturn) {
        NSTask *i = [NSTask new];
        i.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcode-select"];
        i.arguments = @[@"--install"];
        [i launchAndReturnError:nil];
    }
}

- (void)choose:(id)sender {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = YES;                       // Patrician3.exe itself works too
    p.allowsMultipleSelection = NO;
    p.message = @"Choose the Patrician III folder (the one with Patrician3.exe).";
    p.prompt = @"Choose";
    NSString *start = gamePath ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CrossOver/Bottles"];
    p.directoryURL = [NSURL fileURLWithPath:start];
    [p beginSheetModalForWindow:window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = p.URL.path;
        BOOL dir = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&dir] && !dir) path = path.stringByDeletingLastPathComponent;
        if ([self hasGame:path]) [self setGame:path note:@""];
        else [self setStatus:@"That folder has no Patrician3.exe. Choose the folder the game is installed in." error:YES];
    }];
}

- (void)appendLog:(NSString *)s {
    [logView.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:s attributes:@{
        NSFontAttributeName: logView.font, NSForegroundColorAttributeName: NSColor.secondaryLabelColor}]];
    [logView scrollToEndOfDocument:nil];
}

- (void)handleLine:(NSString *)line {
    if ([line hasPrefix:@"@step "]) {
        NSScanner *s = [NSScanner scannerWithString:[line substringFromIndex:6]];
        int n = 0;
        [s scanInt:&n];
        progress.doubleValue = n - 1;
        [self setStatus:[[line substringFromIndex:6 + s.scanLocation] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] error:NO];
    } else if ([line hasPrefix:@"@done "]) {
        installedApp = [line substringFromIndex:6];
    } else if ([line hasPrefix:@"@error "]) {
        lastError = [line substringFromIndex:7];
    } else {
        [self appendLog:[line stringByAppendingString:@"\n"]];
    }
}

- (void)build:(id)sender {
    if (installedApp) {                           // the button reads "Open Patrician III" once it's installed
        [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:installedApp]
                                            configuration:[NSWorkspaceOpenConfiguration configuration] completionHandler:nil];
        [NSApp terminate:nil];
        return;
    }
    buildButton.enabled = chooseButton.enabled = NO;
    lastError = nil;
    progress.doubleValue = 0;
    logView.string = @"";
    NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs"];
    [NSFileManager.defaultManager createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *logPath = [logs stringByAppendingPathComponent:@"Patrician III Setup.log"];
    [NSFileManager.defaultManager createFileAtPath:logPath contents:nil attributes:nil];
    logFile = [NSFileHandle fileHandleForWritingAtPath:logPath];
    activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityIdleSystemSleepDisabled
                                                            reason:@"Building Patrician III"];

    // install.sh runs in its own process group, so quitting Setup can stop it and every compiler it started.
    NSString *script = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"payload/install.sh"];
    int fds[2];
    pipe(fds);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    char *argv[] = {"/bin/sh", (char *)script.fileSystemRepresentation, (char *)gamePath.fileSystemRepresentation, NULL};
    int err = posix_spawn(&child, "/bin/sh", &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);
    if (err) {
        close(fds[0]);
        [self finished:-1];
        return;
    }
    pending = [NSMutableData new];
    NSFileHandle *out = [[NSFileHandle alloc] initWithFileDescriptor:fds[0] closeOnDealloc:YES];
    out.readabilityHandler = ^(NSFileHandle *h) {
        NSData *data = h.availableData;
        if (!data.length) {                           // end of output: the script has finished
            h.readabilityHandler = nil;
            int st = 0;
            waitpid(self->child, &st, 0);
            dispatch_async(dispatch_get_main_queue(), ^{
                [self flushPending:YES];
                self->child = 0;
                [self finished:WIFEXITED(st) ? WEXITSTATUS(st) : -1];
            });
            return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self->logFile writeData:data];
            [self->pending appendData:data];
            [self flushPending:NO];
        });
    };
}

- (void)flushPending:(BOOL)all {
    const char *bytes = pending.bytes;
    NSUInteger start = 0, n = pending.length;
    for (NSUInteger i = 0; i < n; ++i)
        if (bytes[i] == '\n') {
            NSString *line = [[NSString alloc] initWithBytes:bytes + start length:i - start encoding:NSUTF8StringEncoding] ?: @"";
            [self handleLine:line];
            start = i + 1;
        }
    if (all && start < n) {
        [self handleLine:[[NSString alloc] initWithBytes:bytes + start length:n - start encoding:NSUTF8StringEncoding] ?: @""];
        start = n;
    }
    [pending replaceBytesInRange:NSMakeRange(0, start) withBytes:NULL length:0];
}

- (void)finished:(int)code {
    [logFile closeFile];
    logFile = nil;
    if (activity) [NSProcessInfo.processInfo endActivity:activity];
    activity = nil;
    chooseButton.enabled = YES;
    buildButton.enabled = YES;
    if (code == 0 && installedApp) {
        progress.doubleValue = STEPS;
        [self setStatus:[NSString stringWithFormat:@"Done. Patrician III is in %@.",
                         [installedApp.stringByDeletingLastPathComponent stringByAbbreviatingWithTildeInPath]] error:NO];
        buildButton.title = @"Open Patrician III";
        chooseButton.enabled = NO;
        [NSWorkspace.sharedWorkspace selectFile:installedApp inFileViewerRootedAtPath:@""];
    } else {
        [self setStatus:lastError ?: @"The build stopped. The log is in ~/Library/Logs/Patrician III Setup.log." error:YES];
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    if (!child) return NSTerminateNow;
    NSAlert *a = [NSAlert new];
    a.messageText = @"Stop building Patrician III?";
    a.informativeText = @"The game isn't installed yet. You can run Setup again later.";
    [a addButtonWithTitle:@"Stop and Quit"];
    [a addButtonWithTitle:@"Continue"];
    if ([a runModal] != NSAlertFirstButtonReturn) return NSTerminateCancel;
    kill(-child, SIGTERM);
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Setup *s = [Setup new];
        app.delegate = s;
        [app run];
    }
    return 0;
}
