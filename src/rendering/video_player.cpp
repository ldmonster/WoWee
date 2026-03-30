#include "rendering/video_player.hpp"
#include "core/logger.hpp"
#include <GL/glew.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace wowee {
namespace rendering {

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() {
    close();
}

bool VideoPlayer::open(const std::string& path) {
    if (!path.empty() && sourcePath == path && formatCtx) return true;
    close();

    sourcePath = path;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
        LOG_WARNING("VideoPlayer: failed to open ", path);
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOG_WARNING("VideoPlayer: failed to read stream info for ", path);
        avformat_close_input(&fmt);
        return false;
    }

    int streamIndex = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (streamIndex < 0) {
        LOG_WARNING("VideoPlayer: no video stream in ", path);
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecParameters* codecpar = fmt->streams[streamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LOG_WARNING("VideoPlayer: unsupported codec for ", path);
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        avformat_close_input(&fmt);
        return false;
    }
    if (avcodec_parameters_to_context(ctx, codecpar) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVFrame* f = av_frame_alloc();
    AVFrame* rgb = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!f || !rgb || !pkt) {
        if (pkt) av_packet_free(&pkt);
        if (rgb) av_frame_free(&rgb);
        if (f) av_frame_free(&f);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    width = ctx->width;
    height = ctx->height;
    if (width <= 0 || height <= 0) {
        av_packet_free(&pkt);
        av_frame_free(&rgb);
        av_frame_free(&f);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
    rgbBuffer.resize(static_cast<size_t>(bufferSize));
    av_image_fill_arrays(rgb->data, rgb->linesize,
                         rgbBuffer.data(), AV_PIX_FMT_RGB24, width, height, 1);

    SwsContext* sws = sws_getContext(width, height, ctx->pix_fmt,
                                     width, height, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        av_packet_free(&pkt);
        av_frame_free(&rgb);
        av_frame_free(&f);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVRational fr = fmt->streams[streamIndex]->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) {
        fr = fmt->streams[streamIndex]->r_frame_rate;
    }
    double fps = (fr.num > 0 && fr.den > 0) ? static_cast<double>(fr.num) / fr.den : 30.0;
    if (fps <= 0.0) fps = 30.0;
    frameTime = 1.0 / fps;
    accumulator = 0.0;
    eof = false;

    formatCtx = fmt;
    codecCtx = ctx;
    frame = f;
    rgbFrame = rgb;
    packet = pkt;
    swsCtx = sws;
    videoStreamIndex = streamIndex;

    textureReady = false;
    return true;
}

void VideoPlayer::close() {
    if (textureId) {
        glDeleteTextures(1, &textureId);
        textureId = 0;
    }
    textureReady = false;

    if (packet) {
        av_packet_free(reinterpret_cast<AVPacket**>(&packet));
        packet = nullptr;
    }
    if (rgbFrame) {
        av_frame_free(reinterpret_cast<AVFrame**>(&rgbFrame));
        rgbFrame = nullptr;
    }
    if (frame) {
        av_frame_free(reinterpret_cast<AVFrame**>(&frame));
        frame = nullptr;
    }
    if (codecCtx) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&codecCtx));
        codecCtx = nullptr;
    }
    if (formatCtx) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&formatCtx));
        formatCtx = nullptr;
    }
    if (swsCtx) {
        sws_freeContext(reinterpret_cast<SwsContext*>(swsCtx));
        swsCtx = nullptr;
    }
    videoStreamIndex = -1;
    width = 0;
    height = 0;
    rgbBuffer.clear();
}

void VideoPlayer::update(float deltaTime) {
    if (!formatCtx || !codecCtx) return;
    accumulator += deltaTime;
    while (accumulator >= frameTime) {
        if (!decodeNextFrame()) break;
        accumulator -= frameTime;
    }
}

bool VideoPlayer::decodeNextFrame() {
    AVFormatContext* fmt = reinterpret_cast<AVFormatContext*>(formatCtx);
    AVCodecContext* ctx = reinterpret_cast<AVCodecContext*>(codecCtx);
    AVFrame* f = reinterpret_cast<AVFrame*>(frame);
    AVFrame* rgb = reinterpret_cast<AVFrame*>(rgbFrame);
    AVPacket* pkt = reinterpret_cast<AVPacket*>(packet);
    SwsContext* sws = reinterpret_cast<SwsContext*>(swsCtx);

    // Cap iterations to prevent infinite spinning on corrupt/truncated video
    // files where av_read_frame fails but av_seek_frame succeeds, looping
    // endlessly through the same corrupt region.
    constexpr int kMaxDecodeAttempts = 500;
    for (int attempt = 0; attempt < kMaxDecodeAttempts; ++attempt) {
        int ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (av_seek_frame(fmt, videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(ctx);
                continue;
            }
            return false;
        }

        if (pkt->stream_index != videoStreamIndex) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        ret = avcodec_receive_frame(ctx, f);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            continue;
        }
        if (ret < 0) {
            continue;
        }

        sws_scale(sws,
                  f->data, f->linesize,
                  0, ctx->height,
                  rgb->data, rgb->linesize);

        uploadFrame();
        return true;
    }
    LOG_WARNING("Video decode: exceeded ", kMaxDecodeAttempts, " attempts — possible corrupt file");
    return false;
}

void VideoPlayer::uploadFrame() {
    if (width <= 0 || height <= 0) return;
    if (!textureId) {
        glGenTextures(1, &textureId);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, rgbBuffer.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        textureReady = true;
        return;
    }

    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RGB, GL_UNSIGNED_BYTE, rgbBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    textureReady = true;
}

} // namespace rendering
} // namespace wowee
