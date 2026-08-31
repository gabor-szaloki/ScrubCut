#include "MacReveal.h"

#import <AppKit/AppKit.h>

bool MacRevealFilesInFinder(const std::vector<std::string>& utf8Paths) {
    @autoreleasepool {
        NSMutableArray<NSURL*>* urls =
            [NSMutableArray arrayWithCapacity:utf8Paths.size()];
        for (const std::string& p : utf8Paths) {
            NSString* s = [NSString stringWithUTF8String:p.c_str()];
            if (s) [urls addObject:[NSURL fileURLWithPath:s]];
        }
        if (urls.count == 0) return false;
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:urls];
        return true;
    }
}
