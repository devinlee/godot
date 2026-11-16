#include "surface_export.h"
#include <limits>
#include <atomic>

namespace meton::bridge {
namespace wire = meton::engine::surface;
namespace {
std::atomic<std::uint64_t> committed_bytes{0}, abandoned_bytes{0};
/** LUID 按无符号位模式封包，避免有符号 HighPart 扩展或 JS 数值截断。 */
std::uint64_t luid_value(LUID value) {
	return std::uint64_t(value.LowPart) | (std::uint64_t(static_cast<std::uint32_t>(value.HighPart)) << 32);
}
bool same_binding(const wire::Packet &a, const wire::Packet &b) {
	return a.instance_generation == b.instance_generation && a.surface_generation == b.surface_generation && a.surface_id == b.surface_id;
}
}

SurfaceExport::~SurfaceExport() {
	bool offered = false;
	for (const auto &slot : slots) offered = offered || slot.offered;
	if (offered && !released) {
		// 析构不是复制/释放确认。异常撤销时保留原句柄和 COM 引用到进程退出，
		// 防止迟到的消费者按已复用的整数复制另一个对象；保留量仍占固定预算。
		abandoned_bytes.fetch_add(bytes);
		for (auto &slot : slots) { (void)slot.texture.Detach(); (void)slot.fence.Detach(); }
		bytes = 0;
	} else clear();
}
std::uint64_t SurfaceExport::retained_bytes() { return abandoned_bytes.load(); }
std::shared_ptr<void> SurfaceExport::reserve_auxiliary(std::uint64_t bytes, std::uint64_t limit) {
	if (!bytes || bytes > limit) return {};
	auto used = committed_bytes.load();
	do { if (used > limit || bytes > limit - used) return {}; }
	while (!committed_bytes.compare_exchange_weak(used, used + bytes));
	return std::shared_ptr<void>(new std::uint64_t(bytes), [](void *value) {
		auto *count = static_cast<std::uint64_t *>(value); committed_bytes.fetch_sub(*count); delete count;
	});
}
void SurfaceExport::clear() {
	for (auto &slot : slots) {
		if (slot.texture_handle) CloseHandle(slot.texture_handle);
		if (slot.fence_handle) CloseHandle(slot.fence_handle);
		slot.texture_handle = nullptr; slot.fence_handle = nullptr;
		slot.texture.Reset(); slot.fence.Reset();
	}
	if (bytes) committed_bytes.fetch_sub(bytes);
	bytes = 0; prepared = false;
}

HRESULT SurfaceExport::prepare(ID3D12Device *device, const Packet &value, const SurfaceLimits &limits) {
	if (prepared || release_started || released || !device || !value.instance_generation || !value.surface_generation || !value.surface_id ||
		!value.width || !value.height || value.width > limits.max_width || value.height > limits.max_height ||
		!value.slot_count || value.slot_count > limits.max_slots || value.slot_count > slots.size() ||
		(value.format != 1 && value.format != 2) || value.initial_fence != 0 || value.adapter_luid != luid_value(device->GetAdapterLuid())) return E_INVALIDARG;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = value.width; desc.Height = value.height;
	desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = value.format == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
	if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > limits.max_gpu_bytes / value.slot_count) return E_OUTOFMEMORY;
	const auto reserved = allocation.SizeInBytes * value.slot_count;
	auto used = committed_bytes.load();
	do { if (used > limits.max_gpu_bytes || reserved > limits.max_gpu_bytes - used) return E_OUTOFMEMORY; }
	while (!committed_bytes.compare_exchange_weak(used, used + reserved));
	bytes = reserved;
	D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
	shape = value;
	for (std::uint32_t i = 0; i < shape.slot_count; ++i) {
		auto &slot = slots[i];
		HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.texture));
		if (SUCCEEDED(result)) result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&slot.fence));
		// D3D12 只支持 GENERIC_ALL；边界由对象集合、私有通道和接收方复验形成。
		if (SUCCEEDED(result)) result = device->CreateSharedHandle(slot.texture.Get(), nullptr, GENERIC_ALL, nullptr, &slot.texture_handle);
		if (SUCCEEDED(result)) result = device->CreateSharedHandle(slot.fence.Get(), nullptr, GENERIC_ALL, nullptr, &slot.fence_handle);
		if (FAILED(result)) { clear(); return result; }
	}
	prepared = true; return S_OK;
}

bool SurfaceExport::offer(std::uint32_t index, std::uint64_t sequence, Packet &output) {
	if (!prepared || release_started || index >= shape.slot_count || !sequence) return false;
	auto &slot = slots[index];
	if (slot.copied) return false;
	if (slot.offered) { if (slot.offer.sequence != sequence) return false; output = slot.offer; return true; }
	Packet value = shape; value.kind = wire::Kind::offer; value.slot_index = index; value.sequence = sequence;
	value.texture_handle = reinterpret_cast<std::uintptr_t>(slot.texture_handle); value.fence_handle = reinterpret_cast<std::uintptr_t>(slot.fence_handle);
	std::array<std::uint8_t, 104> encoded{}; std::size_t written = 0;
	if (wire::encode(value, encoded.data(), encoded.size(), written) != wire::Error::ok) return false;
	if (sequence <= last_sequence) return false;
	slot.offer = value; slot.offered = true; last_sequence = sequence; output = value; return true;
}

bool SurfaceExport::acknowledge(const Packet &ack) {
	if (!prepared || release_started || ack.kind != wire::Kind::offer_ack || !same_binding(shape, ack) || ack.slot_index >= shape.slot_count || ack.status != 0) return false;
	auto &slot = slots[ack.slot_index];
	if (!slot.offered || ack.sequence != slot.offer.sequence) return false;
	if (slot.copied) return true;
	CloseHandle(slot.texture_handle); CloseHandle(slot.fence_handle);
	slot.texture_handle = nullptr; slot.fence_handle = nullptr; slot.copied = true; return true;
}

bool SurfaceExport::admitted() const {
	if (!prepared || release_started) return false;
	for (std::uint32_t i = 0; i < shape.slot_count; ++i) if (!slots[i].copied) return false;
	return true;
}
ID3D12Resource *SurfaceExport::texture(std::uint32_t index) const { return admitted() && index < shape.slot_count ? slots[index].texture.Get() : nullptr; }
ID3D12Fence *SurfaceExport::fence(std::uint32_t index) const { return admitted() && index < shape.slot_count ? slots[index].fence.Get() : nullptr; }

bool SurfaceExport::begin_frame(std::uint32_t index, std::uint64_t sequence, std::uint64_t document_version, Packet &output) {
	if (!admitted() || index >= shape.slot_count || sequence <= last_sequence) return false;
	auto &slot = slots[index];
	if (slot.writing || slot.produced >= UINT64_MAX - 2) return false;
	const auto expected = slot.produced ? slot.produced + 1 : 0;
	// UINT64_MAX 是设备移除，不是整个时间线都已完成；超前值也不能成为新一轮的许可。
	if (slot.fence->GetCompletedValue() != expected) return false;
	Packet value{}; value.kind = wire::Kind::frame; value.instance_generation = shape.instance_generation;
	value.surface_generation = shape.surface_generation; value.surface_id = shape.surface_id; value.sequence = sequence;
	value.slot_index = index; value.document_version = document_version; value.fence_value = slot.produced ? slot.produced + 2 : 1;
	slot.frame = value; slot.writing = true; slot.publish_attempted = false; last_sequence = sequence; output = value; return true;
}

HRESULT SurfaceExport::publish_frame(ID3D12CommandQueue *queue, const Packet &frame) {
	if (!admitted() || !queue || frame.kind != wire::Kind::frame || !same_binding(shape, frame) || frame.slot_index >= shape.slot_count) return E_INVALIDARG;
	auto &slot = slots[frame.slot_index];
	if (!slot.writing || slot.publish_attempted || frame.sequence != slot.frame.sequence || frame.fence_value != slot.frame.fence_value ||
		frame.document_version != slot.frame.document_version || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
	Microsoft::WRL::ComPtr<ID3D12Device> device, owner;
	HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
	if (FAILED(result) || luid_value(device->GetAdapterLuid()) != shape.adapter_luid) return E_INVALIDARG;
	result = slot.texture->GetDevice(IID_PPV_ARGS(&owner));
	if (FAILED(result) || owner.Get() != device.Get()) return E_INVALIDARG;
	slot.publish_attempted = true;
	result = queue->Signal(slot.fence.Get(), frame.fence_value);
	if (SUCCEEDED(result)) { slot.produced = frame.fence_value; slot.writing = false; }
	return result;
}

bool SurfaceExport::release_completed_request(std::uint64_t sequence, Packet &output) {
	if (release_started) { if (sequence != releasing.sequence) return false; output = releasing; return true; }
	if (!admitted() || sequence <= last_sequence) return false;
	for (std::uint32_t index = 0; index < shape.slot_count; ++index) {
		const auto &slot = slots[index];
		if (slot.writing || slot.fence->GetCompletedValue() != (slot.produced ? slot.produced + 1 : 0)) return false;
	}
	for (auto &slot : slots) { slot.texture.Reset(); slot.fence.Reset(); }
	releasing = {}; releasing.kind = wire::Kind::release; releasing.instance_generation = shape.instance_generation;
	releasing.surface_generation = shape.surface_generation; releasing.surface_id = shape.surface_id; releasing.sequence = sequence;
	release_started = true; output = releasing; return true;
}

bool SurfaceExport::release_unstarted_request(std::uint64_t sequence, Packet &output) {
	if (release_started) { if (sequence != releasing.sequence) return false; output = releasing; return true; }
	if (!admitted() || !sequence) return false;
	for (const auto &slot : slots) if (slot.offered && sequence <= slot.offer.sequence) return false;
	for (std::uint32_t index = 0; index < shape.slot_count; ++index) if (slots[index].writing || slots[index].produced || slots[index].fence->GetCompletedValue() != 0) return false;
	// 消费方回收时必须已没有生产方引用；额度仍保守保持到释放确认。
	for (auto &slot : slots) { slot.texture.Reset(); slot.fence.Reset(); }
	releasing = {}; releasing.kind = wire::Kind::release; releasing.instance_generation = shape.instance_generation;
	releasing.surface_generation = shape.surface_generation; releasing.surface_id = shape.surface_id; releasing.sequence = sequence;
	release_started = true; output = releasing; return true;
}
bool SurfaceExport::release_acknowledge(const Packet &ack) {
	if (!release_started || ack.kind != wire::Kind::release_ack || !same_binding(releasing, ack) || ack.sequence != releasing.sequence) return false;
	if (!released) { clear(); released = true; }
	return true;
}
}
