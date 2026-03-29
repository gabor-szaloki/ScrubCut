#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

#include <string>

namespace ff {

// Convert an FFmpeg error code to a human-readable string.
inline std::string ErrorString(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

} // namespace ff
