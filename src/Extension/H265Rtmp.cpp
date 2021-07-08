﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "H265Rtmp.h"
#ifdef ENABLE_MP4
#include "mpeg4-hevc.h"
#endif//ENABLE_MP4

namespace mediakit{

H265RtmpDecoder::H265RtmpDecoder() {
    _h265frame = obtainFrame();
}

H265Frame::Ptr H265RtmpDecoder::obtainFrame() {
    auto frame = FrameImp::create<H265Frame>();
    frame->_prefix_size = 4;
    return frame;
}

#ifdef ENABLE_MP4
/**
 * 返回不带0x00 00 00 01头的sps
 * @return
 */
static bool getH265ConfigFrame(const RtmpPacket &thiz,string &frame) {
    if (thiz.getMediaType() != FLV_CODEC_H265) {
        return false;
    }
    if (!thiz.isCfgFrame()) {
        return false;
    }
    if (thiz.buffer.size() < 6) {
        WarnL << "bad H265 cfg!";
        return false;
    }

    auto extra = thiz.buffer.data() + 5;
    auto bytes = thiz.buffer.size() - 5;

    struct mpeg4_hevc_t hevc = {0};
    if (mpeg4_hevc_decoder_configuration_record_load((uint8_t *) extra, bytes, &hevc) > 0) {
        uint8_t *config = new uint8_t[bytes * 2];
        int size = mpeg4_hevc_to_nalu(&hevc, config, bytes * 2);
        if (size > 4) {
            frame.assign((char *) config + 4, size - 4);
        }
        delete [] config;
        return size > 4;
    }
    return false;
}
#endif

void H265RtmpDecoder::inputRtmp(const RtmpPacket::Ptr &pkt) {
    if (pkt->isCfgFrame()) {
#ifdef ENABLE_MP4
        string config;
        if(getH265ConfigFrame(*pkt,config)){
            onGetH265(config.data(), config.size(), pkt->time_stamp , pkt->time_stamp);
        }
#else
        WarnL << "请开启MP4相关功能并使能\"ENABLE_MP4\",否则对H265-RTMP支持不完善";
#endif
        return;
    }

    if (pkt->buffer.size() > 9) {
        auto total_len = pkt->buffer.size();
        size_t offset = 5;
        uint8_t *cts_ptr = (uint8_t *) (pkt->buffer.data() + 2);
        int32_t cts = (((cts_ptr[0] << 16) | (cts_ptr[1] << 8) | (cts_ptr[2])) + 0xff800000) ^ 0xff800000;
        auto pts = pkt->time_stamp + cts;
        while (offset + 4 < total_len) {
            uint32_t frame_len;
            memcpy(&frame_len, pkt->buffer.data() + offset, 4);
            frame_len = ntohl(frame_len);
            offset += 4;
            if (frame_len + offset > total_len) {
                break;
            }
            onGetH265(pkt->buffer.data() + offset, frame_len, pkt->time_stamp, pts);
            offset += frame_len;
        }
    }
}

inline void H265RtmpDecoder::onGetH265(const char* pcData, size_t iLen, uint32_t dts,uint32_t pts) {
    if(iLen == 0){
        return;
    }
#if 1
    _h265frame->_dts = dts;
    _h265frame->_pts = pts;
    _h265frame->_buffer.assign("\x00\x00\x00\x01", 4);  //添加265头
    _h265frame->_buffer.append(pcData, iLen);

    //写入环形缓存
    RtmpCodec::inputFrame(_h265frame);
    _h265frame = obtainFrame();
#else
    //防止内存拷贝，这样产生的265帧不会有0x00 00 01头
    auto frame = std::make_shared<H265FrameNoCacheAble>((char *)pcData,iLen,dts,pts,0);
    RtmpCodec::inputFrame(frame);
#endif
}

////////////////////////////////////////////////////////////////////////

H265RtmpEncoder::H265RtmpEncoder(const Track::Ptr &track) {
    _track = dynamic_pointer_cast<H265Track>(track);
}

void H265RtmpEncoder::makeConfigPacket(){
    if (_track && _track->ready()) {
        //尝试从track中获取sps pps信息
        _sps = _track->getSps();
        _pps = _track->getPps();
        _vps = _track->getVps();
    }

    if (!_sps.empty() && !_pps.empty() && !_vps.empty()) {
        //获取到sps/pps
        makeVideoConfigPkt();
        _got_config_frame = true;
    }
}

void H265RtmpEncoder::inputFrame(const Frame::Ptr &frame) {
    auto data = frame->data() + frame->prefixSize();
    auto len = frame->size() - frame->prefixSize();
    auto type = H265_TYPE(((uint8_t*)data)[0]);

    switch (type) {
        case H265Frame::NAL_SPS: {
            if (!_got_config_frame) {
                _sps = string(data, len);
                makeConfigPacket();
            }
            break;
        }
        case H265Frame::NAL_PPS: {
            if (!_got_config_frame) {
                _pps = string(data, len);
                makeConfigPacket();
            }
            break;
        }
        case H265Frame::NAL_VPS: {
            if (!_got_config_frame) {
                _vps = string(data, len);
                makeConfigPacket();
            }
            break;
        }
        case H265Frame::NAL_AUD:
        case H265Frame::NAL_SEI_PREFIX:
        case H265Frame::NAL_SEI_SUFFIX: return;
        default: break;
    }

    if(frame->configFrame() && _rtmp_packet && _has_vcl){
        // sps pps flush frame
        RtmpCodec::inputRtmp(_rtmp_packet);
        _rtmp_packet = nullptr;
        _has_vcl = false;
    }

    if (_rtmp_packet && (_rtmp_packet->time_stamp != frame->dts() || (_has_vcl && type >= H265Frame::NAL_TRAIL_R && type <= H265Frame::NAL_RSV_IRAP_VCL23 && (data[2] >> 7 & 0x01) != 0))) {
        RtmpCodec::inputRtmp(_rtmp_packet);
        _has_vcl = false;
        _rtmp_packet = nullptr;
    }

    if (type >= H265Frame::NAL_TRAIL_R && type <= H265Frame::NAL_RSV_IRAP_VCL23) {
        _has_vcl = true;
    }

    if (!_rtmp_packet) {
        //I or P or B frame
        int8_t flags = FLV_CODEC_H265;
        bool is_config = false;
        flags |= (((frame->configFrame() || frame->keyFrame()) ? FLV_KEY_FRAME : FLV_INTER_FRAME) << 4);
        // todo 必须是IDR帧才能是关键帧，否则有可能开始帧会花屏 SPS PPS VPS 打头的是一般I帧，但不一定是IDR帧
        //  RtmpCodec::inputRtmp 时需要判断 是否是IDR帧,做出相应的修改
        _rtmp_packet = RtmpPacket::create();
        _rtmp_packet->buffer.push_back(flags);
        _rtmp_packet->buffer.push_back(!is_config);
        int32_t cts = frame->pts() - frame->dts();
        if (cts < 0) {
            cts = 0;
        }
        cts = htonl(cts);
        _rtmp_packet->buffer.append((char *) &cts + 1, 3);
        _rtmp_packet->chunk_id = CHUNK_VIDEO;
        _rtmp_packet->stream_index = STREAM_MEDIA;
        _rtmp_packet->time_stamp = frame->dts();
        _rtmp_packet->type_id = MSG_VIDEO;
    }
    uint32_t size = htonl((uint32_t) len);
    _rtmp_packet->buffer.append((char *) &size, 4);
    _rtmp_packet->buffer.append(data, len);
    _rtmp_packet->body_size = _rtmp_packet->buffer.size();
}

void H265RtmpEncoder::makeVideoConfigPkt() {
#ifdef ENABLE_MP4
    int8_t flags = FLV_CODEC_H265;
    flags |= (FLV_KEY_FRAME << 4);
    bool is_config = true;
    auto rtmpPkt = RtmpPacket::create();
    //header
    rtmpPkt->buffer.push_back(flags);
    rtmpPkt->buffer.push_back(!is_config);
    //cts
    rtmpPkt->buffer.append("\x0\x0\x0", 3);

    struct mpeg4_hevc_t hevc = {0};
    string vps_sps_pps = string("\x00\x00\x00\x01", 4) + _vps +
                         string("\x00\x00\x00\x01", 4) + _sps +
                         string("\x00\x00\x00\x01", 4) + _pps;
    h265_annexbtomp4(&hevc, vps_sps_pps.data(), (int)vps_sps_pps.size(), NULL, 0, NULL, NULL);
    uint8_t extra_data[1024];
    int extra_data_size = mpeg4_hevc_decoder_configuration_record_save(&hevc, extra_data, sizeof(extra_data));
    if (extra_data_size == -1) {
        WarnL << "生成H265 extra_data 失败";
        return;
    }
    //HEVCDecoderConfigurationRecord
    rtmpPkt->buffer.append((char *)extra_data, extra_data_size);
    rtmpPkt->body_size = rtmpPkt->buffer.size();
    rtmpPkt->chunk_id = CHUNK_VIDEO;
    rtmpPkt->stream_index = STREAM_MEDIA;
    rtmpPkt->time_stamp = 0;
    rtmpPkt->type_id = MSG_VIDEO;
    RtmpCodec::inputRtmp(rtmpPkt);
#else
    WarnL << "请开启MP4相关功能并使能\"ENABLE_MP4\",否则对H265-RTMP支持不完善";
#endif
}

}//namespace mediakit
