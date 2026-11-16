#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <memory>
#include "surface_wire.generated.hpp"

namespace meton::bridge {

/** 数值由受信装配的固定策略提供；此 C++ 机制不注册为项目脚本 API。 */
struct SurfaceLimits {
	std::uint32_t max_width = 0, max_height = 0, max_slots = 0;
	std::uint64_t max_gpu_bytes = 0;
};

/** G03 拥有的一代表面；原始 NT 句柄保持到同一 offer 的复制确认。 */
class SurfaceExport final {
public:
	using Packet = meton::engine::surface::Packet;
	SurfaceExport() = default;
	~SurfaceExport();
	SurfaceExport(const SurfaceExport &) = delete;
	SurfaceExport &operator=(const SurfaceExport &) = delete;
	/** 初始资源由锁定设备创建；任何失败均回收尚未交出的材料。 */
	HRESULT prepare(ID3D12Device *device, const Packet &shape, const SurfaceLimits &limits);
	/** 重试返回原封包，不能重新创建或复用其中的整数句柄。 */
	bool offer(std::uint32_t slot, std::uint64_t sequence, Packet &output);
	/** 只认原 offer 的完整关联；重复确认不会二次关闭句柄。 */
	bool acknowledge(const Packet &ack);
	/** 只有全部槽都已复制确认，渲染桥才能开始写入。 */
	bool admitted() const;
	/** 仅引擎内部渲染桥使用；消费者不能通过协议选择其他对象。 */
	ID3D12Resource *texture(std::uint32_t slot) const;
	ID3D12Fence *fence(std::uint32_t slot) const;
	/** 精确看到上一帧消费值才预留；未提交的预留也不能被另一个帧覆盖。 */
	bool begin_frame(std::uint32_t slot, std::uint64_t sequence, std::uint64_t document_version, Packet &output);
	/** 调用者先在同一队列提交写入并转回 COMMON；失败后该槽保持未知，不盲目重发。 */
	HRESULT publish_frame(ID3D12CommandQueue *queue, const Packet &frame);
	/** 全部生产和消费均已确认后结束源引用；释放回执仍按原请求去重。 */
	bool release_completed_request(std::uint64_t sequence, Packet &output);
	/** 仅未生产帧且全部复制确认的集合；先结束生产方引用，再请求消费者释放。 */
	bool release_unstarted_request(std::uint64_t sequence, Packet &output);
	/** 原释放确认到达才归还本代资源；无确认不得把超时当作已释放。 */
	bool release_acknowledge(const Packet &ack);
	std::uint64_t allocated_bytes() const { return bytes; }
	/** 未确认的对象只随进程最终退出回收；后续分配仍计入这部分额度。 */
	static std::uint64_t retained_bytes();
	/** 导出器拥有的中间图像也计入同一预算；最后实际资源解除后才销毁此预留。 */
	static std::shared_ptr<void> reserve_auxiliary(std::uint64_t bytes, std::uint64_t limit);
private:
	struct Slot {
		Microsoft::WRL::ComPtr<ID3D12Resource> texture;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence;
		HANDLE texture_handle = nullptr, fence_handle = nullptr;
		Packet offer{};
		bool offered = false, copied = false;
		Packet frame{};
		std::uint64_t produced = 0;
		bool writing = false, publish_attempted = false;
	};
	std::array<Slot, 3> slots;
	Packet shape{}, releasing{};
	std::uint64_t bytes = 0;
	std::uint64_t last_sequence = 0;
	bool prepared = false, release_started = false, released = false;
	/** 仅用于创建失败、已确认释放及进程最终销毁；不作为运行中的超时回收。 */
	void clear();
};
}
