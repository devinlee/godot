#include "surface_capture.h"
#include "scene_input.h"
#include "core/os/thread.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include <algorithm>
#include <array>
#include <limits>

namespace meton::bridge {
using Microsoft::WRL::ComPtr;
namespace wire = meton::engine::surface;
namespace {
std::uint64_t luid_of(LUID value) { return std::uint64_t(value.LowPart) | (std::uint64_t(static_cast<std::uint32_t>(value.HighPart)) << 32); }
}
struct SurfaceCapture::Impl {
	CaptureContext context;
	CaptureStatus facts;
	std::shared_ptr<SurfaceTransport> transport;
	RD *rd = nullptr;
	Thread::ID owner = 0;
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12Fence> completion;
	bool enhanced = false;
	RID viewport_texture;
	std::uint64_t version = 0, sequence = 0, generation = 0, timeline = 0;
	std::uint64_t document_epoch = 0;
	struct Slot {
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12GraphicsCommandList7> list7;
		std::uint64_t stamp = 0;
	};
	struct Group {
		SurfaceExport exported;
		wire::Packet shape;
		std::uint64_t extent_sequence = 0;
		std::uint64_t document_epoch = 0;
		RD::DataFormat format = RD::DATA_FORMAT_MAX;
		std::shared_ptr<void> allocation;
		ComPtr<ID3D12Resource> intermediate;
		RID wrapped;
		std::array<Slot, 3> slots;
		bool retiring = false, release_sent = false, released = false;
		ULONGLONG deadline = 0;
	};
	std::array<std::unique_ptr<Group>, 2> groups;
	Group *active = nullptr, *recorded = nullptr;
	wire::Packet pending{};
	bool dispatched_unknown = false;
	bool recorded_submitted = false;

	bool on_thread() const { return owner == Thread::get_caller_id(); }
	void fail(HRESULT error) { if (SUCCEEDED(facts.failure)) facts.failure = error; facts.stopping = true; transport->stop(); }
	bool checked(HRESULT error) { if (FAILED(error)) { fail(error); return false; } return true; }
	bool next(std::uint64_t &value) { if (value == UINT64_MAX) { fail(E_BOUNDS); return false; } ++value; return true; }
	bool local_done(const Group &group) const {
		if (dispatched_unknown) return false;
		const auto value = completion->GetCompletedValue();
		if (value == UINT64_MAX) return false;
		for (const auto &slot : group.slots) if (slot.stamp > value) return false;
		return recorded != &group;
	}
	/** wrapper 不拥有外部 COM 对象；GPU 完成后解除 RID，再归还本组件的实际分配。 */
	void clear(Group &group) {
		if (recorded == &group) recorded = nullptr;
		if (group.wrapped.is_valid()) { rd->free_rid(group.wrapped); group.wrapped = RID(); }
		for (auto &slot : group.slots) { slot.list7.Reset(); slot.list.Reset(); slot.allocator.Reset(); }
		group.intermediate.Reset(); group.allocation.reset();
	}
	bool create_group(const RD::TextureFormat &format) {
		auto found = std::find_if(groups.begin(), groups.end(), [](const auto &group) { return !group; });
		if (found == groups.end()) return false;
		if (!format.width || !format.height || format.width > context.limits.max_width || format.height > context.limits.max_height ||
			format.depth != 1 || format.array_layers != 1 || format.samples != RD::TEXTURE_SAMPLES_1 ||
			(format.format != RD::DATA_FORMAT_R8G8B8A8_UNORM && format.format != RD::DATA_FORMAT_B8G8R8A8_UNORM)) { fail(E_INVALIDARG); return false; }
		auto group = std::make_unique<Group>(); group->format = format.format; group->extent_sequence = facts.extent_sequence;
		group->document_epoch = document_epoch;
		auto &shape = group->shape; shape.instance_generation = context.instance_generation; shape.surface_id = context.surface_id;
		if (!next(generation)) return false;
		shape.surface_generation = generation; shape.adapter_luid = context.adapter_luid; shape.width = format.width; shape.height = format.height;
		shape.format = format.format == RD::DATA_FORMAT_R8G8B8A8_UNORM ? 1 : 2; shape.slot_count = static_cast<std::uint16_t>(context.limits.max_slots);
		if (!checked(group->exported.prepare(device.Get(), shape, context.limits))) return false;
		D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = format.width; desc.Height = format.height;
		desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = shape.format == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
		group->allocation = SurfaceExport::reserve_auxiliary(allocation.SizeInBytes, context.limits.max_gpu_bytes);
		if (!group->allocation) { fail(E_OUTOFMEMORY); return false; }
		D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
		// Godot 的外部纹理包装从 RENDER_TARGET 开始跟踪；资源由 G03 自己保有，便于精确归还预算。
		if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&group->intermediate)))) return false;
		group->wrapped = rd->texture_create_from_extension(RD::TEXTURE_TYPE_2D, format.format, RD::TEXTURE_SAMPLES_1,
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT,
			reinterpret_cast<std::uint64_t>(group->intermediate.Get()), format.width, format.height, 1, 1, 1);
		if (!group->wrapped.is_valid()) { fail(E_FAIL); return false; }
		for (unsigned i = 0; i < shape.slot_count; ++i) {
			auto &slot = group->slots[i];
			if (!checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
				!checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list))) ||
				!checked(slot.list->Close()) || (enhanced && !checked(slot.list.As(&slot.list7)))) { clear(*group); return false; }
		}
		// 先纳入所有者，再发送任何 offer；发送中断也保持所有句柄到确认或进程结束。
		if (active) { active->retiring = true; active->deadline = GetTickCount64() + context.operation_timeout_ms; }
		active = group.get(); *found = std::move(group); active->deadline = GetTickCount64() + context.operation_timeout_ms;
		for (unsigned i = 0; i < shape.slot_count; ++i) {
			wire::Packet packet{};
			if (!next(sequence) || !active->exported.offer(i, sequence, packet) || !transport->send(packet)) { fail(E_FAIL); return false; }
		}
		return true;
	}
	/** 本帧 RD 已提交后，不论导出是否成功，都用私有栅栏封住本地 GPU 工作。 */
	bool seal() {
		if (!recorded || !recorded_submitted || !next(timeline)) return false;
		const HRESULT result = queue->Signal(completion.Get(), timeline);
		if (!checked(result)) { dispatched_unknown = true; return false; }
		recorded->slots[pending.slot_index].stamp = timeline; dispatched_unknown = false; recorded = nullptr; return true;
	}
	/** RD 保持中间纹理为 COPY_DEST；原生读取结束后恢复它，避免绕过其状态跟踪。 */
	void barriers(Group &group, Slot &slot, bool before) {
		ID3D12Resource *source = group.intermediate.Get(), *target = group.exported.texture(pending.slot_index);
		if (enhanced) {
			std::array<D3D12_TEXTURE_BARRIER, 2> barriers{};
			for (auto &barrier : barriers) barrier.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[0].pResource = source; barriers[1].pResource = target;
			barriers[0].LayoutBefore = before ? D3D12_BARRIER_LAYOUT_COPY_DEST : D3D12_BARRIER_LAYOUT_COPY_SOURCE;
			barriers[0].LayoutAfter = before ? D3D12_BARRIER_LAYOUT_COPY_SOURCE : D3D12_BARRIER_LAYOUT_COPY_DEST;
			barriers[1].LayoutBefore = before ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_COPY_DEST;
			barriers[1].LayoutAfter = before ? D3D12_BARRIER_LAYOUT_COPY_DEST : D3D12_BARRIER_LAYOUT_COMMON;
			for (unsigned i = 0; i < 2; ++i) {
				barriers[i].SyncBefore = before ? D3D12_BARRIER_SYNC_NONE : D3D12_BARRIER_SYNC_COPY;
				barriers[i].SyncAfter = before ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_NONE;
				const auto access = i == 0 ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_COPY_DEST;
				barriers[i].AccessBefore = before ? D3D12_BARRIER_ACCESS_NO_ACCESS : access;
				barriers[i].AccessAfter = before ? access : D3D12_BARRIER_ACCESS_NO_ACCESS;
			}
			D3D12_BARRIER_GROUP description{}; description.Type = D3D12_BARRIER_TYPE_TEXTURE; description.NumBarriers = 2; description.pTextureBarriers = barriers.data();
			slot.list7->Barrier(1, &description);
		} else {
			std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
			for (auto &barrier : barriers) { barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
			barriers[0].Transition.pResource = source; barriers[1].Transition.pResource = target;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			if (!before) for (auto &barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			slot.list->ResourceBarrier(2, barriers.data());
		}
	}
	void poll() {
		if (facts.stopped) return;
		const bool removed = FAILED(device->GetDeviceRemovedReason());
		if (removed) fail(DXGI_ERROR_DEVICE_REMOVED);
		if (!removed && facts.stopping && recorded_submitted && recorded) seal();
		for (const auto &packet : transport->take()) {
			if (packet.kind == wire::Kind::view_input) {
				if (!context.input_queue_limit || packet.instance_generation != context.instance_generation || packet.surface_id != context.surface_id) {
					fail(E_INVALIDARG); continue;
				}
				update_input_scope(facts);
				queue_scene_input(packet, transport, context.input_queue_limit, context.operation_timeout_ms);
				continue;
			}
			if (packet.kind == wire::Kind::view_extent) {
				if (!context.follows_extent || packet.instance_generation != context.instance_generation || packet.surface_id != context.surface_id ||
						packet.surface_generation != 0 || !packet.width || !packet.height || packet.width > context.limits.max_width ||
						packet.height > context.limits.max_height) { fail(E_INVALIDARG); continue; }
				if (packet.sequence < facts.extent_sequence) continue;
				if (packet.sequence == facts.extent_sequence && (packet.width != facts.width || packet.height != facts.height)) { fail(E_INVALIDARG); continue; }
				facts.extent_sequence = packet.sequence; facts.width = packet.width; facts.height = packet.height;
				continue;
			}
			if (packet.kind == wire::Kind::view_state) {
				if (packet.instance_generation != context.instance_generation || packet.surface_id != context.surface_id || packet.surface_generation != 0) { fail(E_INVALIDARG); continue; }
				if (packet.sequence < facts.control_sequence) continue;
				if (packet.sequence == facts.control_sequence && (packet.visible != 0) != facts.visible) { fail(E_INVALIDARG); continue; }
				facts.control_sequence = packet.sequence; facts.visible = packet.visible != 0; continue;
			}
			bool accepted = false;
			for (auto &group : groups) if (group && group->shape.surface_generation == packet.surface_generation) {
				if (packet.kind == wire::Kind::offer_ack) accepted = group->exported.acknowledge(packet);
				else if (packet.kind == wire::Kind::release_ack) { accepted = group->exported.release_acknowledge(packet); if (accepted) group->released = true; }
			}
			if (!accepted) fail(E_INVALIDARG);
		}
		const auto pipe = transport->status();
		if (pipe.stopping) {
			if (!facts.stopping) for (auto &group : groups) if (group) group->deadline = GetTickCount64() + context.operation_timeout_ms;
			// 正常管道结束走自退；格式、方向、积压和超时仍报告失败，不能改写成正常 EOF。
			if (pipe.failure != TransportFailure::none && pipe.failure != TransportFailure::peer_closed &&
				!(pipe.failure == TransportFailure::read && (pipe.native_error == ERROR_BROKEN_PIPE || pipe.native_error == ERROR_NO_DATA || pipe.native_error == ERROR_HANDLE_EOF)))
				fail(HRESULT_FROM_WIN32(pipe.native_error ? pipe.native_error : ERROR_INVALID_DATA));
			facts.stopping = true;
		}
		for (auto &group : groups) if (group) {
			if ((!group->exported.admitted() || group->retiring || facts.stopping) && !group->released && GetTickCount64() >= group->deadline)
				fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			if ((group->retiring || facts.stopping) && !group->release_sent && local_done(*group)) {
				wire::Packet packet{};
				if (sequence < UINT64_MAX && group->exported.release_completed_request(sequence + 1, packet)) {
					++sequence; group->release_sent = true; group->deadline = GetTickCount64() + context.operation_timeout_ms;
					if (!transport->send(packet)) facts.stopping = true;
				}
			}
			if (group->release_sent && !group->released && GetTickCount64() >= group->deadline) fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			if (group->released || (transport->status().stopping && (removed || local_done(*group)))) {
				// 远端未确认时 SurfaceExport 的析构保留其对象；本地命令结束后只清理自己持有的中间资源。
				if (active == group.get()) active = nullptr;
				clear(*group); group.reset();
			}
		}
		facts.retained_bytes = SurfaceExport::retained_bytes();
		if (facts.stopping && !groups[0] && !groups[1]) { transport->stop(); facts.stopped = transport->status().stopped; }
		if (context.input_queue_limit) update_input_scope(facts);
	}
};

SurfaceCapture::SurfaceCapture(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
SurfaceCapture::~SurfaceCapture() {
	stop(); poll();
	// 析构不证明 GPU 已完成。未知材料连同 RD wrapper 保持到进程最终结束。
	if (impl->groups[0] || impl->groups[1] || impl->recorded || impl->dispatched_unknown) (void)impl.release();
}
std::unique_ptr<SurfaceCapture> SurfaceCapture::create(const CaptureContext &context, std::shared_ptr<SurfaceTransport> transport) {
	RD *rd = RD::get_singleton();
	if (!rd || OS::get_singleton()->get_current_rendering_driver_name() != "d3d12" || !transport || !context.instance_generation || !context.surface_id || !context.adapter_luid || !context.operation_timeout_ms ||
		!context.limits.max_slots || context.limits.max_slots > 3 || !context.limits.max_width || !context.limits.max_height || !context.limits.max_gpu_bytes ||
		!context.max_fps || context.max_fps > 1000 || !context.shutdown_timeout_ms) return {};
	auto value = std::make_unique<Impl>(); value->rd = rd; value->context = context; value->transport = std::move(transport); value->owner = Thread::get_caller_id();
	value->device = reinterpret_cast<ID3D12Device *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE));
	value->queue = reinterpret_cast<ID3D12CommandQueue *>(rd->get_driver_resource(RD::DRIVER_RESOURCE_COMMAND_QUEUE));
	if (!value->device || !value->queue || luid_of(value->device->GetAdapterLuid()) != context.adapter_luid || value->queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return {};
	ComPtr<ID3D12Device> queue_device;
	if (FAILED(value->queue->GetDevice(IID_PPV_ARGS(&queue_device))) || queue_device.Get() != value->device.Get() ||
		FAILED(value->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&value->completion)))) return {};
	D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
	if (SUCCEEDED(value->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)))) value->enhanced = options.EnhancedBarriersSupported;
	return std::unique_ptr<SurfaceCapture>(new SurfaceCapture(std::move(value)));
}
bool SurfaceCapture::source(RID texture, std::uint64_t version, std::uint64_t epoch) {
	if (!impl->on_thread() || impl->facts.stopping || (!texture.is_valid() && !epoch)) return false;
	if (epoch) impl->document_epoch = epoch;
	impl->viewport_texture = texture; impl->version = version; return true;
}
void SurfaceCapture::capture() {
	auto &self = *impl; if (!self.on_thread()) return;
	self.poll(); if (self.facts.stopping || !self.facts.visible || !self.viewport_texture.is_valid()) return;
	if (self.recorded) { self.fail(E_UNEXPECTED); return; }
	RID texture = RenderingServer::get_singleton()->texture_get_rd_texture(self.viewport_texture);
	if (!texture.is_valid()) return;
	auto format = self.rd->texture_get_format(texture);
	if (self.context.follows_extent) {
		// Godot 的可渲染视口每边至少 2 像素。单像素布局保留左侧/顶侧有效区域，输出纹理仍是请求大小。
		if (!self.facts.extent_sequence || format.width != MAX(2U, self.facts.width) || format.height != MAX(2U, self.facts.height)) return;
		format.width = self.facts.width; format.height = self.facts.height;
	}
	if (!self.active || self.active->shape.width != format.width || self.active->shape.height != format.height ||
			self.active->format != format.format || self.active->extent_sequence != self.facts.extent_sequence || self.active->document_epoch != self.document_epoch) {
		if (!self.create_group(format)) { ++self.facts.skipped; return; }
	}
	auto &group = *self.active;
	if (!group.exported.admitted()) { ++self.facts.skipped; return; }
	const auto completed = self.completion->GetCompletedValue();
	if (completed == UINT64_MAX) { self.fail(DXGI_ERROR_DEVICE_REMOVED); return; }
	for (unsigned i = 0; i < group.shape.slot_count; ++i) {
		if (group.slots[i].stamp > completed || self.sequence == UINT64_MAX) continue;
		if (!group.exported.begin_frame(i, self.sequence + 1, self.version, self.pending)) continue;
		++self.sequence; self.recorded = &group; self.recorded_submitted = false;
		if (self.rd->texture_copy(texture, group.wrapped, Vector3(), Vector3(), Vector3(format.width, format.height, 1), 0, 0, 0, 0) != OK) self.fail(E_FAIL);
		return;
	}
	++self.facts.skipped;
}
void SurfaceCapture::submitted() {
	auto &self = *impl; if (!self.on_thread() || !self.recorded) return;
	auto &group = *self.recorded; auto &slot = group.slots[self.pending.slot_index];
	self.recorded_submitted = true;
	// 即使本帧录制后收到取消，RD 已提交的中间写入也必须有本地完成证明。
	if (self.facts.stopping) { self.seal(); return; }
	if (!self.checked(slot.allocator->Reset()) || !self.checked(slot.list->Reset(slot.allocator.Get(), nullptr))) return;
	self.barriers(group, slot, true); slot.list->CopyResource(group.exported.texture(self.pending.slot_index), group.intermediate.Get()); self.barriers(group, slot, false);
	if (!self.checked(slot.list->Close())) return;
	ID3D12CommandList *lists[]{slot.list.Get()}; self.dispatched_unknown = true; self.queue->ExecuteCommandLists(1, lists);
	const HRESULT published = group.exported.publish_frame(self.queue.Get(), self.pending);
	if (!self.seal()) return;
	if (!self.checked(published)) return;
	auto frame = self.pending;
	if (self.context.follows_extent) { frame.kind = wire::Kind::extent_frame; frame.extent_sequence = group.extent_sequence; }
	if (!self.facts.stopping && self.transport->send(frame)) {
		++self.facts.published; self.facts.surface_generation = group.shape.surface_generation; self.facts.document_version = self.pending.document_version;
		self.facts.document_epoch = group.document_epoch;
		if (self.context.input_queue_limit) {
			update_input_scope(self.facts);
			record_input_frame(frame, self.context.input_queue_limit);
		}
	} else self.fail(E_ABORT);
}
void SurfaceCapture::stop() {
	if (!impl->on_thread() || impl->facts.stopping) return;
	impl->facts.stopping = true;
	for (auto &group : impl->groups) if (group) group->deadline = GetTickCount64() + impl->context.operation_timeout_ms;
}
void SurfaceCapture::poll() { if (impl->on_thread()) impl->poll(); }
CaptureStatus SurfaceCapture::status() const { return impl->on_thread() ? impl->facts : CaptureStatus{true, false, E_ACCESSDENIED}; }
}
