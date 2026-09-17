// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "FileDragController.h"
#import <AppKit/AppKit.h>
#include <QFile>
#include <QUrl>
#include <QWidget>
#include <thread>

static pid_t dragSourcePid = 0;

static void configureDragOverlay(NSWindow *window)
{
  // Drag targets must stay at the shared edge while Finder is the active app,
  // including when Stage Manager is arranging background application windows.
  auto behavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                  NSWindowCollectionBehaviorFullScreenAuxiliary;
  if (@available(macOS 13.0, *))
    behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
  window.collectionBehavior = behavior;
  window.hidesOnDeactivate = NO;
  window.level = NSPopUpMenuWindowLevel;
}

static QList<QUrl> fileUrlsFromDragPasteboard(NSPasteboard *pasteboard)
{
  // Use AppKit's current drag data directly. Qt's Carbon pasteboard bridge can
  // retain an invalid drag reference after the source drag is cancelled.
  NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[[NSURL class]]
                                                   options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  QList<QUrl> result;
  if (urls.count > 256)
    return result;
  for (NSURL *url in urls) {
    if (!url.isFileURL || !url.filePathURL.path)
      return {};
    result.push_back(QUrl::fromLocalFile(QString::fromNSString(url.filePathURL.path)));
  }
  return result;
}

@interface DFFileDragEdge : NSView {
@public
  std::function<void(const QList<QUrl> &)> offer;
  std::function<void()> ended;
  NSInteger sequence;
}
@end

@implementation DFFileDragEdge
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
  sequence = sender.draggingSequenceNumber;
  const auto urls = fileUrlsFromDragPasteboard(sender.draggingPasteboard);
  const auto operations = sender.draggingSourceOperationMask;
  qInfo("file drag: AppKit edge entered, files=%d, operations=%lu", static_cast<int>(urls.size()),
        static_cast<unsigned long>(operations));
  if (urls.isEmpty() || !(operations & NSDragOperationCopy))
    return NSDragOperationNone;
  offer(urls);
  return NSDragOperationCopy;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
  return (sender.draggingSourceOperationMask & NSDragOperationCopy) ? NSDragOperationCopy : NSDragOperationNone;
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
  return NO;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
  return NO;
}
- (void)draggingEnded:(id<NSDraggingInfo>)sender
{
  if (sender.draggingSequenceNumber == sequence && ended)
    ended();
}
@end

void configureNativeFileDragEdge(
    QWidget *edge, std::function<void(const QList<QUrl> &)> offer, std::function<void()> ended
)
{
  NSView *view = (__bridge NSView *)(void *)edge->winId();
  DFFileDragEdge *target = nil;
  for (NSView *child in view.subviews) {
    if ([child isKindOfClass:[DFFileDragEdge class]]) {
      target = (DFFileDragEdge *)child;
      break;
    }
  }
  if (!target) {
    target = [[DFFileDragEdge alloc] initWithFrame:view.bounds];
    target.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [target registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    [view addSubview:target];
  }
  target->offer = std::move(offer);
  target->ended = std::move(ended);
  configureDragOverlay(view.window);
  [view.window orderFrontRegardless];
}

bool nativeFileDragButtonDown()
{
  const bool down = CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonLeft);
  static bool wasDown = false;
  if (down && !wasDown)
    dragSourcePid = [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier;
  wasDown = down;
  NSPasteboard *drag = [NSPasteboard pasteboardWithName:NSPasteboardNameDrag];
  static NSInteger lastChange = drag.changeCount;
  if (drag.changeCount != lastChange) {
    lastChange = drag.changeCount;
    qInfo(
        "file drag: native pasteboard changed, types=%d, sessionLeft=%d, hidLeft=%d",
        static_cast<int>(drag.types.count),
        CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft),
        CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonLeft)
    );
  }
  return down;
}
QStringList nativeFileDragPaths(const QList<QUrl> &urls)
{
  // A mouse-down can activate a previously background Finder window. Resolve
  // the originating app after its native drag has reached our nonactivating edge.
  auto *application = [NSWorkspace sharedWorkspace].frontmostApplication;
  dragSourcePid = application.processIdentifier;
  qInfo("file drag: source app=%s, event access=%d", qPrintable(QString::fromNSString(application.bundleIdentifier)), CGPreflightPostEventAccess());
  QStringList paths;
  for (const auto &url : urls) {
    NSURL *native = [NSURL URLWithString:url.toString(QUrl::FullyEncoded).toNSString()];
    if (!native.isFileURL || !native.filePathURL.path)
      return {};
    paths.push_back(QString::fromNSString(native.filePathURL.path));
  }
  return paths;
}
static bool cancelDragInProcess(pid_t pid)
{
  if (pid <= 0 || !CGPreflightPostEventAccess()) {
    qWarning("file drag: cannot cancel the native source drag; event posting access is required");
    return false;
  }
  // Target the app that began the drag. A session-wide Escape can race the
  // screen switch or window activation and reach the wrong drag session.
  CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
  if (!source)
    return false;
  for (bool down : {true, false}) {
    CGEventRef event = CGEventCreateKeyboardEvent(source, 53, down);
    if (event) {
      CGEventSetFlags(event, 0);
      CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x44465231);
      CGEventPostToPid(pid, event);
      CFRelease(event);
    }
  }
  CFRelease(source);
  return true;
}
bool cancelNativeFileDragSource()
{
  return cancelDragInProcess(dragSourcePid);
}

@interface DFFileDragSource : NSObject <NSDraggingSource, NSFilePromiseProviderDelegate> {
@public
  std::shared_ptr<DragFiles> files;
  std::function<void(bool)> finished;
  QMetaObject::Connection connection;
  NSPanel *panel;
  NSMutableArray *pending;
  NSInteger copied;
  BOOL ended;
  BOOL accepted;
  BOOL failed;
  BOOL complete;
  NSTimer *mouseTimer;
  CGPoint lastMousePoint;
  BOOL mousePointSent;
  BOOL mouseReleaseSent;
  NSTimeInterval mouseReleaseTime;
  BOOL mouseDownSent;
  BOOL began;
  NSTimeInterval mouseDownTime;
  BOOL ownsSystemButton;
}
- (void)update;
- (void)updateMouse;
- (void)releaseSystemButton;
- (void)finish:(BOOL)okay;
@end

@interface DFFileDragReceiverView : NSView
@property(weak) DFFileDragSource *source;
@property(strong) NSArray<NSDraggingItem *> *items;
@end

@implementation DFFileDragReceiverView
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
  return YES;
}
- (void)mouseDown:(NSEvent *)event
{
  DFFileDragSource *source = self.source;
  if (!source || source->began || source->complete)
    return;
  source->began = YES;
  NSDraggingSession *session = [self beginDraggingSessionWithItems:self.items event:event source:source];
  if (!session) {
    qWarning("file drag: AppKit could not begin receiver session");
    [source finish:NO];
    return;
  }
  session.animatesToStartingPositionsOnCancelOrFail = NO;
  self.window.ignoresMouseEvents = YES;
}
@end

static NSMutableSet<DFFileDragSource *> *sessions;

@implementation DFFileDragSource
- (void)draggingSession:(NSDraggingSession *)session willBeginAtPoint:(NSPoint)point
{
  qInfo("file drag: AppKit receiver session began, buttons=%lu, hid=%d, session=%d", (unsigned long)[NSEvent pressedMouseButtons], CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonLeft), CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft));
}
- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
  return files->cancelled ? NSDragOperationNone : NSDragOperationCopy;
}
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession *)session
{
  return YES;
}
- (void)draggingSession:(NSDraggingSession *)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation
{
  ended = YES;
  accepted = (operation & NSDragOperationCopy) != 0;
  qInfo("file drag: AppKit receiver ended, operation=%lu", static_cast<unsigned long>(operation));
  [self releaseSystemButton];
  [mouseTimer invalidate];
  mouseTimer = nil;
  [panel orderOut:nil];
  if (!accepted) {
    files->cancelled = true;
    [self finish:NO];
  } else
    [self update];
}
- (NSString *)filePromiseProvider:(NSFilePromiseProvider *)provider fileNameForType:(NSString *)type
{
  return files->entries[[provider.userInfo integerValue]].name.toNSString();
}
- (void)filePromiseProvider:(NSFilePromiseProvider *)provider
          writePromiseToURL:(NSURL *)url
          completionHandler:(void (^)(NSError *))completion
{
  qInfo("file drag: Finder requested promised file %ld", static_cast<long>([provider.userInfo integerValue]));
  if (complete || files->cancelled) {
    completion([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
    return;
  }
  [pending addObject:@{@"index" : provider.userInfo, @"url" : url, @"completion" : [completion copy]}];
  [self update];
}
- (void)finish:(BOOL)okay
{
  if (complete)
    return;
  complete = YES;
  [self releaseSystemButton];
  [mouseTimer invalidate];
  mouseTimer = nil;
  QObject::disconnect(connection);
  for (NSDictionary *item in pending) {
    void (^completion)(NSError *) = item[@"completion"];
    completion([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
  }
  [pending removeAllObjects];
  [panel orderOut:nil];
  auto callback = std::move(finished);
  [sessions removeObject:self];
  callback(okay);
}
- (void)releaseSystemButton
{
  if (!ownsSystemButton)
    return;
  ownsSystemButton = NO;
  CGEventRef position = CGEventCreate(nullptr);
  if (!position)
    return;
  CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, CGEventGetLocation(position), kCGMouseButtonLeft);
  CFRelease(position);
  if (event) {
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x44465231);
    CGEventPost(kCGSessionEventTap, event);
    CFRelease(event);
  }
}
- (void)updateMouse
{
  if (complete || ended)
    return;
  const auto now = [NSProcessInfo processInfo].systemUptime;
  if (!mouseDownSent && now - mouseDownTime > 1) {
    qWarning("file drag: receiver window could not capture the handoff");
    [self finish:NO];
    return;
  }
  if (mouseDownSent && !began) {
    if (files->released || files->cancelled || now - mouseDownTime > 1) {
      qWarning("file drag: receiver did not handle handoff mouse-down");
      [self finish:NO];
    }
    return;
  }
  if (mouseReleaseSent) {
    // A missing native end callback must not leave the remote source busy.
    if (now - mouseReleaseTime > 2) {
      qWarning("file drag: AppKit receiver did not finish after release; cancelling");
      files->cancelled = true;
      cancelDragInProcess([NSProcessInfo processInfo].processIdentifier);
      [self finish:NO];
    }
    return;
  }
  const NSPoint screenPoint = [NSEvent mouseLocation];
  const CGPoint point = NSPointToCGPoint(screenPoint);
  if (!mouseDownSent) {
    if (files->released || files->cancelled) {
      [self finish:NO];
      return;
    }
    // A transparent window is skipped by WindowServer hit testing. Verify
    // that the empty capture panel is the actual mouse target before posting
    // the down needed to establish a native system drag.
    [panel setFrameOrigin:NSMakePoint(screenPoint.x - 16, screenPoint.y - 16)];
    if ([NSWindow windowNumberAtPoint:screenPoint belowWindowWithWindowNumber:0] != panel.windowNumber)
      return;
    CGEventRef position = CGEventCreate(nullptr);
    if (!position)
      return;
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, CGEventGetLocation(position), kCGMouseButtonLeft);
    CFRelease(position);
    if (!event)
      return;
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x44465231);
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
    mouseDownSent = YES;
    mouseDownTime = now;
    ownsSystemButton = YES;
    CGEventPost(kCGSessionEventTap, event);
    CFRelease(event);
    return;
  }
  if (mousePointSent && CGPointEqualToPoint(point, lastMousePoint) && !files->released)
    return;
  // Use the same WindowServer event stream as the initial down. Mixing
  // AppKit-queued window-local moves with system moves repeatedly changes
  // the native drop target even when the cursor stays inside that target.
  if (files->released) {
    [self releaseSystemButton];
    mouseReleaseSent = YES;
    mouseReleaseTime = now;
    qInfo("file drag: delivered release to AppKit receiver");
    return;
  }
  CGEventRef position = CGEventCreate(nullptr);
  if (!position)
    return;
  CGEventRef event = CGEventCreateMouseEvent(
      nullptr, kCGEventLeftMouseDragged, CGEventGetLocation(position), kCGMouseButtonLeft
  );
  CFRelease(position);
  if (!event)
    return;
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x44465231);
  CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
  CGEventSetDoubleValueField(event, kCGMouseEventPressure, 1);
  CGEventPost(kCGSessionEventTap, event);
  CFRelease(event);
  lastMousePoint = point;
  mousePointSent = YES;
}
- (void)update
{
  if (complete)
    return;
  if (files->cancelled) {
    if (!ended) {
      cancelDragInProcess([NSProcessInfo processInfo].processIdentifier);
      return;
    }
    [self finish:NO];
    return;
  }
  if (files->ready && pending.count) {
    NSArray *items = [pending copy];
    [pending removeAllObjects];
    for (NSDictionary *item in items) {
      const auto index = [item[@"index"] integerValue];
      const auto source = files->paths[index];
      const auto destination = QString::fromNSString([item[@"url"] path]);
      void (^completion)(NSError *) = item[@"completion"];
      DFFileDragSource *owner = self;
      // Do disk copying off the AppKit event thread. QFile::copy refuses to
      // replace an existing destination; Finder chooses conflict-safe URLs.
      std::thread([owner, source, destination, completion] {
        const bool okay = QFile::copy(source, destination);
        dispatch_async(dispatch_get_main_queue(), ^{
          completion(
              okay ? nil : [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil]
          );
          owner->copied++;
          owner->failed |= !okay;
          [owner update];
        });
      }).detach();
    }
  }
  if (ended && accepted && copied == files->entries.size())
    [self finish:!failed];
}
@end

void startNativeFileDrag(QWidget *, const std::shared_ptr<DragFiles> &files, std::function<void(bool)> finished)
{
  @autoreleasepool {
    if (!sessions)
      sessions = [NSMutableSet set];
    DFFileDragSource *source = [DFFileDragSource new];
    source->files = files;
    source->finished = std::move(finished);
    source->pending = [NSMutableArray array];
    const NSPoint point = [NSEvent mouseLocation];
    source->panel =
        [[NSPanel alloc] initWithContentRect:NSMakeRect(point.x - 16, point.y - 16, 32, 32)
                                   styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                     backing:NSBackingStoreBuffered
                                       defer:NO];
    source->panel.opaque = NO;
    source->panel.backgroundColor = [NSColor colorWithWhite:0 alpha:0.01];
    configureDragOverlay(source->panel);
    [source->panel orderFrontRegardless];
    DFFileDragReceiverView *view = [[DFFileDragReceiverView alloc] initWithFrame:source->panel.contentView.bounds];
    view.source = source;
    source->panel.contentView = view;
    NSMutableArray *items = [NSMutableArray array];
    for (qsizetype i = 0; i < files->entries.size(); ++i) {
      NSFilePromiseProvider *provider = [[NSFilePromiseProvider alloc] initWithFileType:@"public.data" delegate:source];
      provider.userInfo = @(i);
      NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:provider];
      NSImage *icon =
          [[NSWorkspace sharedWorkspace] iconForFileType:[files->entries[i].name.toNSString() pathExtension]];
      [item setDraggingFrame:NSMakeRect(16, 16 - i * 3, 32, 32) contents:icon];
      [items addObject:item];
    }
    [sessions addObject:source];
    source->connection = QObject::connect(files.get(), &DragFiles::updated, files.get(), [source] { [source update]; });
    view.items = items;
    source->mouseDownTime = [NSProcessInfo processInfo].systemUptime;
    source->mouseTimer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *) {
      [source updateMouse];
    }];
    [[NSRunLoop mainRunLoop] addTimer:source->mouseTimer forMode:NSRunLoopCommonModes];
  }
}
