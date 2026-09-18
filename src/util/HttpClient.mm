// macOS HttpGet on NSURLSession. Built with ARC like every .mm in src/.
#include "util/HttpClient.h"
#include "scrubcut_version.h"

#import <Foundation/Foundation.h>

bool HttpGet(const std::string& url, std::string& outBody, std::string& outError, int timeoutMs) {
    outBody.clear();
    outError.clear();

    @autoreleasepool {
        NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!nsUrl) {
            outError = "invalid URL";
            return false;
        }
        const NSTimeInterval timeout = timeoutMs / 1000.0;

        // Ephemeral: no on-disk cache or cookies.
        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = timeout;    // inactivity
        cfg.timeoutIntervalForResource = timeout;   // whole transfer
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg];

        NSMutableURLRequest* req =
            [NSMutableURLRequest requestWithURL:nsUrl
                                    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                timeoutInterval:timeout];
        [req setValue:@"ScrubCut/" SCRUBCUT_VERSION forHTTPHeaderField:@"User-Agent"];

        // Block until the completion handler (session queue) fires.
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSData* body = nil;
        __block NSError* error = nil;
        __block NSInteger status = 0;
        NSURLSessionDataTask* task = [session
            dataTaskWithRequest:req
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* err) {
                  body = data;
                  error = err;
                  if ([response isKindOfClass:[NSHTTPURLResponse class]])
                      status = ((NSHTTPURLResponse*)response).statusCode;
                  dispatch_semaphore_signal(done);
              }];
        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        if (error) {
            const char* desc = error.localizedDescription.UTF8String;
            outError = desc ? desc : "request failed";
            return false;
        }
        if (status < 200 || status >= 300) {
            outError = "HTTP " + std::to_string(status);
            return false;
        }
        if (body)
            outBody.assign(static_cast<const char*>(body.bytes), body.length);
        return true;
    }
}
