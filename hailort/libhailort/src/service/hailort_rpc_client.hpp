/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file hailort_rpc_client.hpp
 * @brief TODO
 **/

#ifndef HAILO_HAILORT_RPC_CLIENT_HPP_
#define HAILO_HAILORT_RPC_CLIENT_HPP_

#include "hailo/hailort.h"
#include "hailo/expected.hpp"
#include "hailo/device.hpp"
#include "rpc/rpc_definitions.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4244 4267 4127)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <grpcpp/grpcpp.h>
#include "hailort_rpc.grpc.pb.h"
#if defined(_MSC_VER)
#pragma warning( pop )
#else
#pragma GCC diagnostic pop
#endif
#include <memory>

namespace hailort
{

// Higher then default-hrt-timeout so we can differentiate errors
static const std::chrono::milliseconds CONTEXT_TIMEOUT(HAILO_DEFAULT_VSTREAM_TIMEOUT_MS + 500);

class ClientContextWithTimeout : public grpc::ClientContext {
public:
    ClientContextWithTimeout()
    {
        set_deadline(std::chrono::system_clock::now() + CONTEXT_TIMEOUT);
    }
};

class HailoRtRpcClient final {
public:
    HailoRtRpcClient(std::shared_ptr<grpc::Channel> channel)
        : m_stub(ProtoHailoRtRpc::NewStub(channel)) {}

    hailo_status client_keep_alive(uint32_t pid);
    Expected<hailo_version_t> get_service_version();

    Expected<uint32_t> VDevice_create(const hailo_vdevice_params_t &params, uint32_t pid);
    hailo_status VDevice_release(const VDeviceIdentifier &identifier, uint32_t pid);
    Expected<std::vector<std::string>> VDevice_get_physical_devices_ids(const VDeviceIdentifier &identifier);
    Expected<std::vector<std::unique_ptr<Device>>> VDevice_get_physical_devices(const VDeviceIdentifier &identifier);
    Expected<hailo_stream_interface_t> VDevice_get_default_streams_interface(const VDeviceIdentifier &identifier);
    Expected<std::vector<uint32_t>> VDevice_configure(const VDeviceIdentifier &identifier, const Hef &hef, uint32_t pid, const NetworkGroupsParamsMap &configure_params={});

    Expected<uint32_t> ConfiguredNetworkGroup_dup_handle(const NetworkGroupIdentifier &identifier, uint32_t pid);
    hailo_status ConfiguredNetworkGroup_release(const NetworkGroupIdentifier &identifier, uint32_t pid);
    Expected<std::map<std::string, hailo_vstream_params_t>> ConfiguredNetworkGroup_make_input_vstream_params(const NetworkGroupIdentifier &identifier,
        bool quantized, hailo_format_type_t format_type, uint32_t timeout_ms, uint32_t queue_size,
        const std::string &network_name);
    Expected<std::map<std::string, hailo_vstream_params_t>> ConfiguredNetworkGroup_make_output_vstream_params(const NetworkGroupIdentifier &identifier,
        bool quantized, hailo_format_type_t format_type, uint32_t timeout_ms, uint32_t queue_size,
        const std::string &network_name);
    Expected<std::string> ConfiguredNetworkGroup_get_network_group_name(const NetworkGroupIdentifier &identifier);
    Expected<std::string> ConfiguredNetworkGroup_name(const NetworkGroupIdentifier &identifier);
    Expected<std::vector<hailo_network_info_t>> ConfiguredNetworkGroup_get_network_infos(const NetworkGroupIdentifier &identifier);
    Expected<std::vector<hailo_stream_info_t>> ConfiguredNetworkGroup_get_all_stream_infos(const NetworkGroupIdentifier &identifier, const std::string &network_name);
    Expected<hailo_stream_interface_t> ConfiguredNetworkGroup_get_default_stream_interface(const NetworkGroupIdentifier &identifier);
    Expected<std::vector<std::map<std::string, hailo_vstream_params_t>>> ConfiguredNetworkGroup_make_output_vstream_params_groups(const NetworkGroupIdentifier &identifier,
        bool quantized, hailo_format_type_t format_type, uint32_t timeout_ms, uint32_t queue_size);
    Expected<std::vector<std::vector<std::string>>> ConfiguredNetworkGroup_get_output_vstream_groups(const NetworkGroupIdentifier &identifier);
    Expected<std::vector<hailo_vstream_info_t>> ConfiguredNetworkGroup_get_input_vstream_infos(const NetworkGroupIdentifier &identifier, std::string network_name);
    Expected<std::vector<hailo_vstream_info_t>> ConfiguredNetworkGroup_get_output_vstream_infos(const NetworkGroupIdentifier &identifier, std::string network_name);
    Expected<std::vector<hailo_vstream_info_t>> ConfiguredNetworkGroup_get_all_vstream_infos(const NetworkGroupIdentifier &identifier, std::string network_name);
    Expected<bool> ConfiguredNetworkGroup_is_scheduled(const NetworkGroupIdentifier &identifier);
    hailo_status ConfiguredNetworkGroup_set_scheduler_timeout(const NetworkGroupIdentifier &identifier, const std::chrono::milliseconds &timeout,
        const std::string &network_name);
    hailo_status ConfiguredNetworkGroup_set_scheduler_threshold(const NetworkGroupIdentifier &identifier, uint32_t threshold, const std::string &network_name);
    hailo_status ConfiguredNetworkGroup_set_scheduler_priority(const NetworkGroupIdentifier &identifier, uint8_t priority, const std::string &network_name);
    Expected<LatencyMeasurementResult> ConfiguredNetworkGroup_get_latency_measurement(const NetworkGroupIdentifier &identifier, const std::string &network_name);
    Expected<bool> ConfiguredNetworkGroup_is_multi_context(const NetworkGroupIdentifier &identifier);
    Expected<ConfigureNetworkParams> ConfiguredNetworkGroup_get_config_params(const NetworkGroupIdentifier &identifier);
    Expected<std::vector<std::string>> ConfiguredNetworkGroup_get_sorted_output_names(const NetworkGroupIdentifier &identifier);
    Expected<std::vector<std::string>> ConfiguredNetworkGroup_get_stream_names_from_vstream_name(const NetworkGroupIdentifier &identifier, const std::string &vstream_name);
    Expected<std::vector<std::string>> ConfiguredNetworkGroup_get_vstream_names_from_stream_name(const NetworkGroupIdentifier &identifier, const std::string &stream_name);

    Expected<std::vector<uint32_t>> InputVStreams_create(const NetworkGroupIdentifier &identifier,
        const std::map<std::string, hailo_vstream_params_t> &inputs_params, uint32_t pid);
    Expected<std::vector<uint32_t>> OutputVStreams_create(const NetworkGroupIdentifier &identifier,
        const std::map<std::string, hailo_vstream_params_t> &output_params, uint32_t pid);

    Expected<uint32_t> InputVStream_dup_handle(const VStreamIdentifier &identifier, uint32_t pid);
    Expected<uint32_t> OutputVStream_dup_handle(const VStreamIdentifier &identifier, uint32_t pid);
    hailo_status InputVStream_release(const VStreamIdentifier &identifier, uint32_t pid);

    hailo_status OutputVStream_release(const VStreamIdentifier &identifier, uint32_t pid);
    Expected<bool> InputVStream_is_multi_planar(const VStreamIdentifier &identifier);
    hailo_status InputVStream_write(const VStreamIdentifier &identifier, const MemoryView &buffer);
    hailo_status InputVStream_write(const VStreamIdentifier &identifier, const hailo_pix_buffer_t &buffer);
    hailo_status OutputVStream_read(const VStreamIdentifier &identifier, MemoryView buffer);
    Expected<size_t> InputVStream_get_frame_size(const VStreamIdentifier &identifier);
    Expected<size_t> OutputVStream_get_frame_size(const VStreamIdentifier &identifier);

    hailo_status InputVStream_flush(const VStreamIdentifier &identifier);

    Expected<std::string> InputVStream_name(const VStreamIdentifier &identifier);
    Expected<std::string> OutputVStream_name(const VStreamIdentifier &identifier);

    Expected<std::string> InputVStream_network_name(const VStreamIdentifier &identifier);
    Expected<std::string> OutputVStream_network_name(const VStreamIdentifier &identifier);

    hailo_status InputVStream_abort(const VStreamIdentifier &identifier);
    hailo_status OutputVStream_abort(const VStreamIdentifier &identifier);
    hailo_status InputVStream_resume(const VStreamIdentifier &identifier);
    hailo_status OutputVStream_resume(const VStreamIdentifier &identifier);
    hailo_status InputVStream_stop_and_clear(const VStreamIdentifier &identifier);
    hailo_status OutputVStream_stop_and_clear(const VStreamIdentifier &identifier);
    hailo_status InputVStream_start_vstream(const VStreamIdentifier &identifier);
    hailo_status OutputVStream_start_vstream(const VStreamIdentifier &identifier);

    Expected<hailo_format_t> InputVStream_get_user_buffer_format(const VStreamIdentifier &identifier);
    Expected<hailo_format_t> OutputVStream_get_user_buffer_format(const VStreamIdentifier &identifier);

    Expected<hailo_vstream_info_t> InputVStream_get_info(const VStreamIdentifier &identifier);
    Expected<hailo_vstream_info_t> OutputVStream_get_info(const VStreamIdentifier &identifier);

    Expected<bool> InputVStream_is_aborted(const VStreamIdentifier &identifier);
    Expected<bool> OutputVStream_is_aborted(const VStreamIdentifier &identifier);

    hailo_status OutputVStream_set_nms_score_threshold(const VStreamIdentifier &identifier, float32_t threshold);
    hailo_status OutputVStream_set_nms_iou_threshold(const VStreamIdentifier &identifier, float32_t threshold);
    hailo_status OutputVStream_set_nms_max_proposals_per_class(const VStreamIdentifier &identifier, uint32_t max_proposals_per_class);

private:
    void VDevice_convert_identifier_to_proto(const VDeviceIdentifier &identifier, ProtoVDeviceIdentifier *proto_identifier);
    void ConfiguredNetworkGroup_convert_identifier_to_proto(const NetworkGroupIdentifier &identifier, ProtoConfiguredNetworkGroupIdentifier *proto_identifier);
    void VStream_convert_identifier_to_proto(const VStreamIdentifier &identifier, ProtoVStreamIdentifier *proto_identifier);

    std::unique_ptr<ProtoHailoRtRpc::Stub> m_stub;
};

}

#endif // HAILO_HAILORT_RPC_CLIENT_HPP_