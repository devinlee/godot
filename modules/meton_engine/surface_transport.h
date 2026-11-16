#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "surface_wire.generated.hpp"

namespace meton::bridge {
struct TransportLimits {
	std::uint32_t max_queue_messages = 0, operation_timeout_ms = 0;
	bool allow_scene_hello = false;
	std::uint32_t max_document_bytes = 0, max_document_requests = 0;
};
struct SceneDocumentRequest {
	std::uint64_t lane = 0, sequence = 0;
	std::string route, body;
};
enum class TransportFailure { none, peer_closed, read, write, shape, binding, direction, queue_full, deadline, thread };
struct TransportStatus {
	bool stopping = false, stopped = false;
	TransportFailure failure = TransportFailure::none;
	DWORD native_error = 0;
	std::size_t queued_reads = 0, queued_writes = 0;
	std::uint64_t received = 0, written = 0;
};

/** 子端只接创建时继承的同步句柄；渲染线程不执行 ReadFile/WriteFile 或等待停止。 */
class SurfaceTransport final {
public:
	using Packet = meton::engine::surface::Packet;
	/** 接收两个句柄的唯一所有权，失败也关闭；调用方必须来自受信启动装配。 */
	static std::shared_ptr<SurfaceTransport> open(HANDLE commands, HANDLE events, std::uint64_t generation, const TransportLimits &limits);
	~SurfaceTransport();
	/** 有界受理只表示消息入队，不能当作宿主已复制或消费。 */
	bool send(const Packet &packet);
	/** 仅正常场景且启动封印包含控制材料时启用，一代只收一份。 */
	std::string take_hello();
	/** 仅在材料已验、尚未初始化图形的启动线程限时等待，渲染线程不等待。 */
	std::string wait_hello(DWORD timeout_ms);
	bool reply_hello(const std::string &body);
	/** 原生读线程只处理字节；场景查询由主线程处理并沿原号应答。 */
	bool take_document(SceneDocumentRequest &request);
	/** 冻结试播尚未进入主循环时，等待当前租约的启动请求或进程停止。 */
	bool wait_document(SceneDocumentRequest &request, DWORD timeout_ms);
	bool reply_document(const SceneDocumentRequest &request, const std::string &body);
	std::vector<Packet> take();
	TransportStatus status() const;
	/** 只发取消请求；监视线程覆盖检查取消与进入同步 I/O 之间的竞态。 */
	void stop();
	/** 仅原生测试或异步清理使用；渲染/main 回调不能等待。 */
	bool wait_stopped(DWORD timeout_ms) const;
private:
	struct State;
	explicit SurfaceTransport(std::shared_ptr<State> state);
	std::shared_ptr<State> state;
};
}
