/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file core_op.cpp
 * @brief Core-Op module implementation
 **/

#include "hailo/network_group.hpp"
#include "hailo/transform.hpp"
#include "hailo/hailort_defaults.hpp"

#include "common/utils.hpp"
#include "common/runtime_statistics_internal.hpp"

#include "core_op/core_op.hpp"
#include "core_op/resource_manager/resource_manager.hpp"
#include "hef/hef_internal.hpp"
#include "eth/eth_stream.hpp"
#include "vdma/vdma_stream.hpp"
#include "mipi/mipi_stream.hpp"
#include "device_common/control_protocol.hpp"
#include "stream_common/nms_stream.hpp"
#include "stream_common/remote_process_stream.hpp"


namespace hailort
{

CoreOp::CoreOp(
    const ConfigureNetworkParams &config_params, std::shared_ptr<CoreOpMetadata> metadata,
    ActiveCoreOpHolder &active_core_op_holder, hailo_status &status) :
        m_config_params(config_params),
        m_active_core_op_holder(active_core_op_holder),
        m_min_configured_batch_size(get_smallest_configured_batch_size(config_params)),
        m_activation_time_accumulator(),
        m_deactivation_time_accumulator(),
        m_metadata(metadata),
        m_vdevice_core_op_handle(INVALID_CORE_OP_HANDLE)
{
    auto event = Event::create_shared(Event::State::not_signalled);
    if (!event) {
        LOGGER__ERROR("Failed to create activation event");
        status = event.status();
        return;
    }
    m_core_op_activated_event = event.release();

    m_activation_time_accumulator = make_shared_nothrow<FullAccumulator<double>>("activation_time");
    if (nullptr == m_activation_time_accumulator) {
        LOGGER__ERROR("Failed to create activation time accumulator");
        status = HAILO_OUT_OF_HOST_MEMORY;
        return;
    };

    m_deactivation_time_accumulator = make_shared_nothrow<FullAccumulator<double>>("deactivation_time");
    if (nullptr == m_deactivation_time_accumulator) {
        LOGGER__ERROR("Failed to create deactivation time accumulator");
        status = HAILO_OUT_OF_HOST_MEMORY;
        return;
    };

    status = HAILO_SUCCESS;
}

Expected<std::chrono::nanoseconds> get_latency(LatencyMeterPtr &latency_meter, bool clear)
{
    auto hw_latency = latency_meter->get_latency(clear);
    if (HAILO_NOT_AVAILABLE == hw_latency.status()) {
        return make_unexpected(HAILO_NOT_AVAILABLE);
    }
    CHECK_EXPECTED(hw_latency, "Failed getting latency");
    return hw_latency.release();
}

/* Network group base functions */
Expected<LatencyMeasurementResult> CoreOp::get_latency_measurement(const std::string &network_name)
{
    bool clear = ((m_config_params.latency & HAILO_LATENCY_CLEAR_AFTER_GET) == HAILO_LATENCY_CLEAR_AFTER_GET);
    LatencyMeasurementResult result = {};

    auto latency_meters_exp = get_latency_meters();
    CHECK_EXPECTED(latency_meters_exp);
    auto latency_meters = latency_meters_exp.release();

    if (network_name.empty()) {
        if (1 != m_input_streams.size()) {
            return make_unexpected(HAILO_NOT_AVAILABLE);
        }
        std::chrono::nanoseconds latency_sum(0);
        uint32_t measurements_count = 0;
        for (auto &latency_meter_pair : *latency_meters.get()) {
            auto hw_latency = get_latency(latency_meter_pair.second, clear);
            if (HAILO_NOT_AVAILABLE == hw_latency.status()) {
                continue;
            }
            CHECK_EXPECTED(hw_latency);
            latency_sum += hw_latency.value();
            measurements_count++;
        }
        if (0 == measurements_count) {
            LOGGER__DEBUG("No latency measurements was found");
            return make_unexpected(HAILO_NOT_AVAILABLE);
        }
        result.avg_hw_latency = latency_sum / measurements_count;
    } else {
        if(!contains(*latency_meters, network_name)) {
            LOGGER__DEBUG("No latency measurements was found for network {}", network_name);
            return make_unexpected(HAILO_NOT_FOUND);
        }
        auto hw_latency = get_latency(latency_meters->at(network_name), clear);
        if (HAILO_NOT_AVAILABLE == hw_latency.status()) {
            return make_unexpected(HAILO_NOT_AVAILABLE);
        }
        CHECK_EXPECTED(hw_latency);
        result.avg_hw_latency = hw_latency.value();
    }
    return result;
}

hailo_status CoreOp::activate(uint16_t dynamic_batch_size)
{
    auto start_time = std::chrono::steady_clock::now();

    CHECK(!is_scheduled(), HAILO_INVALID_OPERATION,
        "Manually activate a core-op is not allowed when the core-op scheduler is active!");

    // Check that no network is currently activated
    CHECK(!m_active_core_op_holder.is_any_active(), HAILO_INVALID_OPERATION,
        "Cant activate network because a network is already activated");
    m_active_core_op_holder.set(*this);

    auto status = activate_impl(dynamic_batch_size);
    if (HAILO_SUCCESS != status) {
        auto deactivate_status = deactivate_impl();
        if (HAILO_SUCCESS != deactivate_status) {
            LOGGER__ERROR("Failed deactivate {}", deactivate_status);
        }
        m_active_core_op_holder.clear();
    }
    if (HAILO_STREAM_ABORTED_BY_USER == status) {
        return status;
    }
    CHECK_SUCCESS(status);

    const auto elapsed_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_time).count();

    status = m_core_op_activated_event->signal();
    if (HAILO_SUCCESS != status) {
        auto deactivate_status = deactivate_impl();
        if (HAILO_SUCCESS != deactivate_status) {
            LOGGER__ERROR("Failed deactivate {}", deactivate_status);
        }
        m_active_core_op_holder.clear();
    }
    CHECK_SUCCESS(status, "Failed to signal network activation event");

    LOGGER__INFO("Activating {} took {} milliseconds. Note that the function is asynchronous and"
                 " thus the network is not fully activated yet.", name(), elapsed_time_ms);
    m_activation_time_accumulator->add_data_point(elapsed_time_ms);

    return HAILO_SUCCESS;
}

hailo_status CoreOp::deactivate()
{
    const auto start_time = std::chrono::steady_clock::now();

    CHECK(!is_scheduled(), HAILO_INVALID_OPERATION,
        "Manually deactivate a core-op is not allowed when the core-op scheduler is active!");

    auto core_op_ref = m_active_core_op_holder.get();
    CHECK_EXPECTED_AS_STATUS(core_op_ref, "Trying to deactivate while no network is running");

    CHECK(this == std::addressof(core_op_ref->get()), HAILO_INTERNAL_FAILURE,
        "Trying to deactivate different core-op");
    m_active_core_op_holder.clear();

    m_core_op_activated_event->reset();

    auto deactivate_status = deactivate_impl();
    if (HAILO_SUCCESS != deactivate_status) {
        LOGGER__ERROR("Failed deactivating core-op (status {})", deactivate_status);
    }

    const auto elapsed_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_time).count();
    LOGGER__INFO("Deactivating took {} ms", elapsed_time_ms);
    m_deactivation_time_accumulator->add_data_point(elapsed_time_ms);

    return deactivate_status;
}

Expected<LayerInfo> CoreOp::get_layer_info(const std::string &stream_name)
{
    for (const auto &layer_info : m_metadata->get_all_layer_infos()) {
        if (layer_info.is_multi_planar) {
            for (const auto &plane : layer_info.planes) {
                if (plane.name == stream_name) {
                    auto cpy = plane;
                    return cpy;
                }
            }
        }
        if (layer_info.name == stream_name) {
            auto cpy = layer_info;
            return cpy;
        }
    }
    LOGGER__ERROR("Failed to find layer with name {}", stream_name);
    return make_unexpected(HAILO_NOT_FOUND);
}

bool CoreOp::is_nms()
{
    for (auto layer_info : m_metadata->get_output_layer_infos()) {
        if (HAILO_FORMAT_ORDER_HAILO_NMS == layer_info.format.order) {
            return true;
        }
    }
    return false;
}

hailo_status CoreOp::add_input_stream(std::shared_ptr<InputStreamBase> &&stream,
    const hailo_stream_parameters_t &stream_params)
{
    if ((stream_params.flags & HAILO_STREAM_FLAGS_ASYNC) != 0) {
        // When the user forces async streams, we use NOT_OWNING mode.
        auto status = stream->set_buffer_mode(StreamBufferMode::NOT_OWNING);
        CHECK_SUCCESS(status);
    } else {
        // When the user forces async streams, we use OWNING mode.
        auto status = stream->set_buffer_mode(StreamBufferMode::OWNING);
        CHECK_SUCCESS(status);
    }

    m_input_streams.emplace(stream->name(), std::move(stream));
    return HAILO_SUCCESS;
}

hailo_status CoreOp::add_output_stream(std::shared_ptr<OutputStreamBase> &&stream,
    const hailo_stream_parameters_t &stream_params)
{
    if ((stream_params.flags & HAILO_STREAM_FLAGS_ASYNC) != 0) {
        // When the user forces async streams, we use NOT_OWNING mode.
        auto status = stream->set_buffer_mode(StreamBufferMode::NOT_OWNING);
        CHECK_SUCCESS(status);
    } else {
        // When the user forces async streams, we use OWNING mode.
        auto status = stream->set_buffer_mode(StreamBufferMode::OWNING);
        CHECK_SUCCESS(status);
    }

    m_output_streams.emplace(stream->name(), std::move(stream));
    return HAILO_SUCCESS;
}

uint16_t CoreOp::get_smallest_configured_batch_size(const ConfigureNetworkParams &config_params)
{
    // There are two possible situations:
    // 1) All networks in the network group have the same configured (and hence smallest) batch_size =>
    //    We return that batch size.
    // 2) Not all of the networks have the same configured (and hence smallest) batch_size. Currently, when
    //    using dynamic_batch_sizes, all networks will use the same dynamic_batch_size (until HRT-6535 is done).
    //    Hence, we must not set a dynamic_batch_size to a value greater than the smallest configured network
    //    batch_size (e.g. all the resources allocated are for at most the configured network batch_size).

    /* We iterate over all network's batch_sizes to get the non-default min.
       Ignoring HAILO_DEFAULT_BATCH_SIZE as it is not a real batch-value,
       but indicating the scheduler should optimize batches by himself */
    uint16_t min_batch_size = UINT16_MAX;
    for (const auto &network_params_pair : config_params.network_params_by_name) {
        if ((HAILO_DEFAULT_BATCH_SIZE != network_params_pair.second.batch_size) &&
            (network_params_pair.second.batch_size < min_batch_size)) {
            min_batch_size = network_params_pair.second.batch_size;
        }
    }
    return (UINT16_MAX == min_batch_size) ? DEFAULT_ACTUAL_BATCH_SIZE : min_batch_size;
}

const std::string &CoreOp::name() const
{
    return m_metadata->core_op_name();
}

hailo_status CoreOp::activate_low_level_streams()
{
    for (auto &name_pair : m_input_streams) {
        auto status = name_pair.second->activate_stream();
        if (HAILO_STREAM_ABORTED_BY_USER == status) {
            LOGGER__INFO("Stream {} activation failed because it was aborted by user", name_pair.first);
            return status;
        }
        CHECK_SUCCESS(status);
    }
    for (auto &name_pair : m_output_streams) {
        auto status = name_pair.second->activate_stream();
        if (HAILO_STREAM_ABORTED_BY_USER == status) {
            LOGGER__INFO("Stream {} activation failed because it was aborted by user", name_pair.first);
            return status;
        }
        CHECK_SUCCESS(status);
    }

    return HAILO_SUCCESS;
}

hailo_status CoreOp::deactivate_low_level_streams()
{
    // Best effort
    auto status = HAILO_SUCCESS;
    auto deactivate_status = HAILO_UNINITIALIZED;
    for (auto &name_pair : m_input_streams) {
        deactivate_status = name_pair.second->deactivate_stream();
        if (HAILO_SUCCESS != deactivate_status) {
            LOGGER__ERROR("Failed to deactivate input stream {}", name_pair.first);
            status = deactivate_status;
        }
    }
    for (auto &name_pair : m_output_streams) {
        deactivate_status = name_pair.second->deactivate_stream();
        if (HAILO_SUCCESS != deactivate_status) {
            LOGGER__ERROR("Failed to deactivate output stream {}", name_pair.first);
            status = deactivate_status;
        }
    }

    return status;
}

const SupportedFeatures &CoreOp::get_supported_features()
{
    return m_metadata->supported_features();
}

Expected<uint16_t> CoreOp::get_stream_batch_size(const std::string &stream_name)
{
    for (const auto &layer_info : m_metadata->get_all_layer_infos()) {
        auto stream_under_multi_planes_layer = (layer_info.is_multi_planar && std::any_of(layer_info.planes.begin(), layer_info.planes.end(),
            [&stream_name](const auto &plane){ return plane.name == stream_name; }));
        if ((layer_info.name == stream_name) || (stream_under_multi_planes_layer)) {
            for (auto const &network_params_pair : m_config_params.network_params_by_name) {
                if (network_params_pair.first == layer_info.network_name) {
                    auto batch_size = network_params_pair.second.batch_size;
                    return (batch_size == HAILO_DEFAULT_BATCH_SIZE) ? DEFAULT_ACTUAL_BATCH_SIZE : batch_size;
                }
            }
        }
    }
    LOGGER__ERROR("Failed to find batch for stream {}", stream_name);
    return make_unexpected(HAILO_NOT_FOUND);
}

bool CoreOp::is_default_batch_size() const
{
    for (auto const &network_params_pair : m_config_params.network_params_by_name) {
        if (network_params_pair.second.batch_size != HAILO_DEFAULT_BATCH_SIZE) {
            return false;
        }
    }

    return true;
}

Expected<Buffer> CoreOp::get_intermediate_buffer(const IntermediateBufferKey &)
{
    LOGGER__ERROR("Getting intermediate buffer is not supported for this core op");
    return make_unexpected(HAILO_NOT_SUPPORTED);
}

hailo_status CoreOp::wrap_streams_for_remote_process()
{
    for (auto &input_stream_pair : m_input_streams) {
        auto base_stream = input_stream_pair.second;

        auto remote_proc_stream = RemoteProcessInputStream::create(base_stream);
        CHECK_EXPECTED_AS_STATUS(remote_proc_stream);

        input_stream_pair.second = remote_proc_stream.release();
    }

    for (auto &output_stream_pair : m_output_streams) {
        auto base_stream = output_stream_pair.second;

        auto remote_proc_stream = RemoteProcessOutputStream::create(base_stream);
        CHECK_EXPECTED_AS_STATUS(remote_proc_stream);

        output_stream_pair.second = remote_proc_stream.release();
    }

    return HAILO_SUCCESS;
}

bool CoreOp::is_multi_context() const
{
    return m_metadata->supported_features().multi_context;
}

const ConfigureNetworkParams CoreOp::get_config_params() const
{
    return m_config_params;
}

Expected<std::shared_ptr<InputStreamBase>> CoreOp::create_input_stream_from_config_params(Device &device,
    const hailo_stream_parameters_t &stream_params, const std::string &stream_name)
{
    auto layer_info = get_layer_info(stream_name);
    CHECK_EXPECTED(layer_info);

    CHECK_AS_EXPECTED(device.is_stream_interface_supported(stream_params.stream_interface), HAILO_INVALID_OPERATION,
        "Device does not supports the given stream interface streams. Please update input_stream_params for stream {}.",
        stream_name);

    std::shared_ptr<InputStreamBase> input_stream = nullptr;
    switch (stream_params.stream_interface) {
        case HAILO_STREAM_INTERFACE_PCIE:
            // Fallthrough
        case HAILO_STREAM_INTERFACE_INTEGRATED:
            {
                auto input_stream_exp = create_vdma_input_stream(device, stream_name, layer_info.value(), stream_params);
                CHECK_EXPECTED(input_stream_exp);
                input_stream = input_stream_exp.release();
                break;
            }

        case HAILO_STREAM_INTERFACE_ETH:
            {
                auto input_stream_exp = EthernetInputStream::create(device,
                    layer_info.value(), stream_params.eth_input_params, m_core_op_activated_event);
                CHECK_EXPECTED(input_stream_exp);
                input_stream = input_stream_exp.release();
                break;
            }

        case HAILO_STREAM_INTERFACE_MIPI:
            {
                auto input_stream_exp = MipiInputStream::create(device,
                    layer_info.value(), stream_params.mipi_input_params, m_core_op_activated_event);
                CHECK_EXPECTED(input_stream_exp);
                input_stream = input_stream_exp.release();
                break;
            }

        default:
            LOGGER__ERROR("{} interface is not supported.", stream_params.stream_interface);
            return make_unexpected(HAILO_NOT_IMPLEMENTED);
    }

    return input_stream;
}

Expected<std::shared_ptr<InputStreamBase>> CoreOp::create_vdma_input_stream(Device &device, const std::string &stream_name,
    const LayerInfo &layer_info, const hailo_stream_parameters_t &stream_params)
{
    // Make sure the downcast is safe
    CHECK_AS_EXPECTED((Device::Type::INTEGRATED == device.get_type()) || (Device::Type::PCIE == device.get_type()),
        HAILO_INTERNAL_FAILURE, "Invalid device type");
    VdmaDevice *vdma_device = reinterpret_cast<VdmaDevice*>(&device);

    auto vdma_channel_ptr_exp = get_boundary_vdma_channel_by_stream_name(stream_name);
    CHECK_EXPECTED(vdma_channel_ptr_exp, "Failed to get vdma channel for output stream {}", stream_name);

    return VdmaInputStream::create(stream_params.stream_interface, *vdma_device, vdma_channel_ptr_exp.value(),
        layer_info, m_core_op_activated_event);
}

Expected<std::shared_ptr<OutputStreamBase>> CoreOp::create_output_stream_from_config_params(Device &device,
    const hailo_stream_parameters_t &stream_params, const std::string &stream_name)
{
    auto layer_info = get_layer_info(stream_name);
    CHECK_EXPECTED(layer_info);

    CHECK_AS_EXPECTED(device.is_stream_interface_supported(stream_params.stream_interface), HAILO_INVALID_OPERATION,
        "Device does not supports the given stream interface streams. Please update input_stream_params for stream {}.",
        stream_name);

    std::shared_ptr<OutputStreamBase> output_stream = nullptr;
    switch (stream_params.stream_interface) {
        case HAILO_STREAM_INTERFACE_PCIE:
            // Fallthrough
        case HAILO_STREAM_INTERFACE_INTEGRATED:
            {
                auto output_stream_exp = create_vdma_output_stream(device, stream_name, layer_info.value(), stream_params);
                CHECK_EXPECTED(output_stream_exp);
                output_stream = output_stream_exp.release();
                break;
            }

        case HAILO_STREAM_INTERFACE_ETH:
            {
                auto output_stream_exp =  EthernetOutputStream::create(device,
                    layer_info.value(), stream_params.eth_output_params, 
                    m_core_op_activated_event);
                CHECK_EXPECTED(output_stream_exp);
                output_stream = output_stream_exp.release();
                break;
            }

        default:
            LOGGER__ERROR("{} interface is not supported.", stream_params.stream_interface);
            return make_unexpected(HAILO_NOT_IMPLEMENTED);
    }

    if (HAILO_FORMAT_ORDER_HAILO_NMS == layer_info->format.order) {
        // In NMS we create some new stream object that wraps the original stream (and converts
        // bbox/burst reads into frame reads).
        // After HRT-10553 is implemented, we won't need this wrapper anymore.
        auto base_stream = std::move(output_stream);

        const auto batch_size = get_smallest_configured_batch_size(m_config_params);
        const auto max_queue_size = batch_size * MAX_ACTIVE_TRANSFERS_SCALE;

        auto nms_stream = NmsOutputStream::create(base_stream, layer_info.value(), max_queue_size,
            m_core_op_activated_event);
        CHECK_EXPECTED(nms_stream);
        output_stream = nms_stream.release();
    }

    return output_stream;
}

Expected<std::shared_ptr<OutputStreamBase>> CoreOp::create_vdma_output_stream(Device &device, const std::string &stream_name,
    const LayerInfo &layer_info, const hailo_stream_parameters_t &stream_params)
{
    // Make sure the downcast is safe
    CHECK_AS_EXPECTED((Device::Type::INTEGRATED == device.get_type()) || (Device::Type::PCIE == device.get_type()),
        HAILO_INTERNAL_FAILURE, "Invalid device type");
    VdmaDevice *vdma_device = reinterpret_cast<VdmaDevice*>(&device);

    auto batch_size_exp = get_stream_batch_size(stream_name);
    CHECK_EXPECTED(batch_size_exp);

    auto vdma_channel_ptr_exp = get_boundary_vdma_channel_by_stream_name(stream_name);
    CHECK_EXPECTED(vdma_channel_ptr_exp, "Failed to get vdma channel for output stream {}", stream_name);

    return VdmaOutputStream::create(stream_params.stream_interface, *vdma_device, vdma_channel_ptr_exp.value(),
        layer_info, m_core_op_activated_event);
}

hailo_status CoreOp::create_streams_from_config_params(Device &device)
{
    for (const auto &stream_parameters_pair : m_config_params.stream_params_by_name) {
        switch (stream_parameters_pair.second.direction) {
            case HAILO_H2D_STREAM:
                {
                    auto stream = create_input_stream_from_config_params(device,
                        stream_parameters_pair.second,
                        stream_parameters_pair.first);
                    CHECK_EXPECTED_AS_STATUS(stream);

                    auto status = add_input_stream(stream.release(), stream_parameters_pair.second);
                    CHECK_SUCCESS(status);
                }
                break;
            case HAILO_D2H_STREAM:
                {
                    auto stream = create_output_stream_from_config_params(device,
                        stream_parameters_pair.second,
                        stream_parameters_pair.first);
                    CHECK_EXPECTED_AS_STATUS(stream);

                    auto status = add_output_stream(stream.release(), stream_parameters_pair.second);
                    CHECK_SUCCESS(status);
                }
                break;
            default:
                LOGGER__ERROR("stream name {} direction is invalid.", stream_parameters_pair.first);
                return HAILO_INVALID_ARGUMENT;
        }
    }

    return HAILO_SUCCESS;
}

Expected<InputStreamRefVector> CoreOp::get_input_streams_by_network(const std::string &network_name)
{
    auto input_stream_infos = m_metadata->get_input_stream_infos(network_name);
    CHECK_EXPECTED(input_stream_infos);

    InputStreamRefVector result;
    for (auto &stream_info : input_stream_infos.value()) {
        auto stream_ref = get_input_stream_by_name(stream_info.name);
        CHECK_EXPECTED(stream_ref);
        result.emplace_back(stream_ref.release());
    }
    return result;
}

Expected<OutputStreamRefVector> CoreOp::get_output_streams_by_network(const std::string &network_name)
{
    auto output_stream_infos = m_metadata->get_output_stream_infos(network_name);
    CHECK_EXPECTED(output_stream_infos);

    OutputStreamRefVector result;
    for (auto &stream_info : output_stream_infos.value()) {
        auto stream_ref = get_output_stream_by_name(stream_info.name);
        CHECK_EXPECTED(stream_ref);
        result.emplace_back(stream_ref.release());
    }
    return result;
}

InputStreamRefVector CoreOp::get_input_streams()
{
    InputStreamRefVector result;
    for (auto& name_stream_pair : m_input_streams) {
        result.emplace_back(std::ref(*name_stream_pair.second));
    }
    return result;
}

OutputStreamRefVector CoreOp::get_output_streams()
{
    OutputStreamRefVector result;
    for (auto& name_stream_pair : m_output_streams) {
        result.emplace_back(std::ref(*name_stream_pair.second));
    }
    return result;
}

ExpectedRef<InputStreamBase> CoreOp::get_input_stream_by_name(const std::string& name)
{
    auto iterator = m_input_streams.find(name);
    if (m_input_streams.end() == iterator) {
        LOGGER__ERROR("Input stream name {} not found", name);
        return make_unexpected(HAILO_NOT_FOUND);
    }

    return std::ref<InputStreamBase>(*iterator->second);
}

ExpectedRef<OutputStreamBase> CoreOp::get_output_stream_by_name(const std::string& name)
{
    auto iterator = m_output_streams.find(name);
    if (m_output_streams.end() == iterator) {
        LOGGER__ERROR("Output stream name {} not found", name);
        return make_unexpected(HAILO_NOT_FOUND);
    }

    return std::ref<OutputStreamBase>(*iterator->second);
}

std::vector<std::reference_wrapper<InputStream>> CoreOp::get_input_streams_by_interface(
    hailo_stream_interface_t stream_interface)
{
    std::vector<std::reference_wrapper<InputStream>> results;
    for (auto &name_pair : m_input_streams) {
        if (stream_interface == name_pair.second->get_interface()) {
            results.emplace_back(std::ref(*name_pair.second));
        }
    }
    return results;
}

std::vector<std::reference_wrapper<OutputStream>> CoreOp::get_output_streams_by_interface(
    hailo_stream_interface_t stream_interface)
{
    std::vector<std::reference_wrapper<OutputStream>> results;
    for (auto &name_pair : m_output_streams) {
        if (stream_interface == name_pair.second->get_interface()) {
            results.emplace_back(std::ref(*name_pair.second));
        }
    }
    return results;
}

hailo_status CoreOp::wait_for_activation(const std::chrono::milliseconds &timeout)
{
    return m_core_op_activated_event->wait(timeout);
}

Expected<std::vector<hailo_stream_info_t>> CoreOp::get_all_stream_infos(
    const std::string &network_name) const
{
    return m_metadata->get_all_stream_infos(network_name);
}

AccumulatorPtr CoreOp::get_activation_time_accumulator() const
{
    return m_activation_time_accumulator;
}

AccumulatorPtr CoreOp::get_deactivation_time_accumulator() const
{
    return m_deactivation_time_accumulator;
}

Expected<std::shared_ptr<InputStreamBase>> CoreOp::get_shared_input_stream_by_name(const std::string &stream_name)
{
    CHECK_AS_EXPECTED(contains(m_input_streams, stream_name), HAILO_NOT_FOUND, "Input stream {} not found.", stream_name);
    auto stream_ptr = m_input_streams.at(stream_name);
    return stream_ptr;
}

Expected<std::shared_ptr<OutputStreamBase>> CoreOp::get_shared_output_stream_by_name(const std::string &stream_name)
{
    CHECK_AS_EXPECTED(contains(m_output_streams, stream_name), HAILO_NOT_FOUND, "Output stream {} not found.", stream_name);
    auto stream_ptr = m_output_streams.at(stream_name);
    return stream_ptr;
}

} /* namespace hailort */
