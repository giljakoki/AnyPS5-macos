// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include "SceTypes.hpp"
#include "prx/libSceAvPlayer/include/AvPlayer.hpp"

using namespace AvPlayer;

namespace {

Player* ToPlayer(AvPlayerInternal* h) {
    return static_cast<Player*>(h);
}

bool Trace() {
    static const bool enabled = std::getenv("APS5_TRACE_AVPLAYER") != nullptr;
    return enabled;
}

void TraceCount(const char* what, bool value) {
    static unsigned counts[2][2] = {};
    const unsigned which = what[0] == 'v' ? 0u : 1u;
    auto& count = counts[which][value ? 1 : 0];
    if (++count <= 8 || count % 256 == 0) std::fprintf(stderr, "[avplayer] %s -> %d (#%u)\n", what, value ? 1 : 0, count);
}

std::uint32_t VideoBufferCount(std::int32_t requested) {
    return static_cast<std::uint32_t>(std::clamp(requested, 2, 16));
}

constexpr std::size_t Ps5InitDataExSize = 0x230;
constexpr std::size_t Ps5AutoStartOffset = 0x74;
constexpr std::size_t Ps5VideoFrameBuffersOffset = 0x228;

}

#pragma GCC visibility push(default)
extern "C" {

int APS5_VABI sceAvPlayerAddSource(AvPlayerInternal* h, const char* filename) {
    if (!h || !filename) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] AddSource %s\n", filename);
    return ToPlayer(h)->AddSource(filename, SourceTypeUnknown);
}

int APS5_VABI sceAvPlayerAddSourceEx(AvPlayerInternal* h, uint32_t uri_type, const AvPlayerSourceDetails* source_details) {
    if (!h || uri_type != 0 || !source_details || !source_details->uri.name) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] AddSourceEx %.*s type %u\n", static_cast<int>(source_details->uri.length), source_details->uri.name, source_details->source_type);
    return ToPlayer(h)->AddSource(std::string_view(source_details->uri.name, source_details->uri.length), source_details->source_type);
}

int APS5_VABI sceAvPlayerChangeStream(AvPlayerInternal* h, uint32_t old_stream_id, uint32_t new_stream_id) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->ChangeStream(old_stream_id, new_stream_id);
}

int APS5_VABI sceAvPlayerClose(AvPlayerInternal* h) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    delete ToPlayer(h);
    if (Trace()) std::fprintf(stderr, "[avplayer] Close\n");
    return SCE_OK;
}

uint64_t APS5_VABI sceAvPlayerCurrentTime(AvPlayerInternal* h) {
    if (!h) return static_cast<uint64_t>(static_cast<int64_t>(SCE_AVPLAYER_ERROR_INVALID_PARAMS));
    return ToPlayer(h)->CurrentTime();
}

int APS5_VABI sceAvPlayerDisableStream(AvPlayerInternal* h, uint32_t stream_id) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->DisableStream(stream_id);
}

int APS5_VABI sceAvPlayerEnableStream(AvPlayerInternal* h, uint32_t stream_id) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->EnableStream(stream_id);
}

Bool APS5_VABI sceAvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info) {
    if (!h || !audio_info) return false;
    const bool taken = ToPlayer(h)->GetAudioData(*audio_info);
    if (Trace()) TraceCount("audio", taken);
    return taken;
}

int APS5_VABI sceAvPlayerGetStreamInfo(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfo* info) {
    if (!h || !info) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->GetStreamInfo(stream_id, *info);
}

int APS5_VABI sceAvPlayerGetStreamInfoEx(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfoEx* info) {
    if (!h || !info) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->GetStreamInfoEx(stream_id, *info);
}

Bool APS5_VABI sceAvPlayerGetVideoData(AvPlayerInternal* h, AvPlayerFrameInfo* video_info) {
    if (!h || !video_info) return false;
    const bool taken = ToPlayer(h)->GetVideoData(*video_info);
    if (Trace()) TraceCount("video", taken);
    return taken;
}

Bool APS5_VABI sceAvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info) {
    if (!h || !video_info) return false;
    const bool taken = ToPlayer(h)->GetVideoData(*video_info);
    if (Trace()) TraceCount("videoEx", taken);
    if (Trace() && taken) std::fprintf(stderr, "[avplayer] frame %ux%u pitch %u ts %llu data %p\n", video_info->details.video.width, video_info->details.video.height, video_info->details.video.pitch, static_cast<unsigned long long>(video_info->timestamp), video_info->p_data);
    return taken;
}

AvPlayerInternal* APS5_VABI sceAvPlayerInit(AvPlayerInitData* init) {
    if (!init) return nullptr;
    if (Trace()) std::fprintf(stderr, "[avplayer] Init buffers=%d autoStart=%d callback=%p\n", init->num_output_video_framebuffers, init->auto_start ? 1 : 0, reinterpret_cast<void*>(init->event_replacement.event_callback));
    return new Player(*init, VideoBufferCount(init->num_output_video_framebuffers));
}

int APS5_VABI sceAvPlayerInitEx(const AvPlayerInitDataEx* init_ex, AvPlayerInternal** handle) {
    if (!init_ex || !handle) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    AvPlayerInitData init{};
    init.memory_replacement = init_ex->memory_replacement;
    init.file_replacement = init_ex->file_replacement;
    init.event_replacement = init_ex->event_replacement;
    init.debug_level = init_ex->debug_level;
    if (init_ex->this_size >= Ps5InitDataExSize) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(init_ex);
        std::memcpy(&init.num_output_video_framebuffers, bytes + Ps5VideoFrameBuffersOffset, sizeof(init.num_output_video_framebuffers));
        init.auto_start = bytes[Ps5AutoStartOffset] != 0;
    } else {
        init.num_output_video_framebuffers = init_ex->num_output_video_framebuffers;
        init.auto_start = init_ex->auto_start;
    }
    init.default_language = init_ex->default_language;
    if (Trace()) std::fprintf(stderr, "[avplayer] InitEx size=%zu buffers=%d autoStart=%d callback=%p\n", static_cast<std::size_t>(init_ex->this_size), init.num_output_video_framebuffers, init.auto_start ? 1 : 0, reinterpret_cast<void*>(init.event_replacement.event_callback));
    *handle = new Player(init, VideoBufferCount(init.num_output_video_framebuffers));
    return SCE_OK;
}

Bool APS5_VABI sceAvPlayerIsActive(AvPlayerInternal* h) {
    if (!h) return false;
    const bool active = ToPlayer(h)->IsActive();
    if (Trace()) TraceCount("isActive", active);
    return active;
}

int APS5_VABI sceAvPlayerJumpToTime(AvPlayerInternal* h, uint64_t time_ms) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] JumpToTime %llu\n", static_cast<unsigned long long>(time_ms));
    return ToPlayer(h)->JumpToTime(time_ms);
}

int APS5_VABI sceAvPlayerPause(AvPlayerInternal* h) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] Pause\n");
    return ToPlayer(h)->Pause();
}

int APS5_VABI sceAvPlayerPostInit(AvPlayerInternal* h, const AvPlayerPostInitData* post_init) {
    if (!h || !post_init) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] PostInit\n");
    return ToPlayer(h)->PostInit(*post_init);
}

int APS5_VABI sceAvPlayerResume(AvPlayerInternal* h) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] Resume\n");
    return ToPlayer(h)->Resume();
}

int APS5_VABI sceAvPlayerSetAvSyncMode(AvPlayerInternal* h, uint32_t sync_mode) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->SetAvSyncMode(sync_mode);
}

int APS5_VABI sceAvPlayerSetAvailableBandwidth(AvPlayerInternal* h, uint32_t start_bandwidth, uint32_t minimum_bandwidth, uint32_t maximum_bandwidth) {
    (void)start_bandwidth;
    (void)minimum_bandwidth;
    (void)maximum_bandwidth;
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return SCE_OK;
}

int APS5_VABI sceAvPlayerSetLogCallback(void* callback, void* user_data) {
    (void)callback;
    (void)user_data;
    return SCE_OK;
}

int APS5_VABI sceAvPlayerSetLooping(AvPlayerInternal* h, Bool loop) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] SetLooping %d\n", loop ? 1 : 0);
    return ToPlayer(h)->SetLooping(loop != 0);
}

int APS5_VABI sceAvPlayerSetTrickSpeed(AvPlayerInternal* h, int32_t trick_speed) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->SetTrickSpeed(trick_speed);
}

int APS5_VABI sceAvPlayerStart(AvPlayerInternal* h) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] Start\n");
    return ToPlayer(h)->Start();
}

int APS5_VABI sceAvPlayerStartEx(AvPlayerInternal* h, const void* start_info_ex) {
    (void)start_info_ex;
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] StartEx\n");
    return ToPlayer(h)->Start();
}

int APS5_VABI sceAvPlayerStop(AvPlayerInternal* h) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    if (Trace()) std::fprintf(stderr, "[avplayer] Stop\n");
    return ToPlayer(h)->Stop();
}

int APS5_VABI sceAvPlayerStreamCount(AvPlayerInternal* h) {
    if (!h) return SCE_AVPLAYER_ERROR_INVALID_PARAMS;
    return ToPlayer(h)->StreamCount();
}

}
#pragma GCC visibility pop
