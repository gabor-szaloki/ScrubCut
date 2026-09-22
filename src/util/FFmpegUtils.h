#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
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

#include <cstdint>
#include <string>

namespace ff {

// Convert an FFmpeg error code to a human-readable string.
inline std::string ErrorString(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

// Timeline seconds to stream ticks. Deliberately the same truncating
// expression as Player::SyncSeekAndDecode and Demuxer::Seek, so a mark
// resolves to the same frame on export as on screen. Treat the result as a
// lower bound ("first frame at or after"): a clock time is exactly
// pts * av_q2d(tb) and dividing it back can land one tick low.
inline int64_t SecondsToPts(double sec, AVRational tb) {
    return static_cast<int64_t>(sec / av_q2d(tb));
}

} // namespace ff
