#pragma once
#include "core/templates/rid.h"
#include "surface_export.h"
#include "surface_transport.h"
#include <memory>

namespace meton::bridge {
struct CaptureContext {
	std::uint64_t instance_generation = 0, surface_id = 0, adapter_luid = 0;
	SurfaceLimits limits;
	std::uint32_t operation_timeout_ms = 0;
	std::uint32_t max_fps = 60, shutdown_timeout_ms = 10000;
	bool follows_extent = false;
	std::uint32_t input_queue_limit = 0;
};
struct CaptureStatus {
	bool stopping = false, stopped = false;
	HRESULT failure = S_OK;
	std::uint64_t published = 0, skipped = 0, surface_generation = 0, document_version = 0;
	std::uint64_t retained_bytes = 0;
	bool visible = false;
	std::uint64_t control_sequence = 0;
	std::uint64_t extent_sequence = 0;
	std::uint32_t width = 0, height = 0;
	std::uint64_t document_epoch = 0;
};

/** 仅 G03 受信调用者与渲染线程持有；不注册脚本类或公开窗口/资源句柄。 */
class SurfaceCapture final {
public:
	static std::unique_ptr<SurfaceCapture> create(const CaptureContext &context, std::shared_ptr<SurfaceTransport> transport);
	~SurfaceCapture();
	/** 来源 RID 和版本由场景权威给定，同一线程换源不改变已经记录的帧版本。 */
	bool source(RID viewport_texture, std::uint64_t document_version, std::uint64_t document_epoch = 0);
	/** 渲染线程：视口已画完、RD 尚未提交时记录本帧复制。 */
	void capture();
	/** 渲染线程：RD 提交以后才交给同一原生队列。 */
	void submitted();
	/** 停止只关闭新帧准入；继续 poll 确认本地 GPU 与远端释放。 */
	void stop();
	void poll();
	CaptureStatus status() const;
private:
	struct Impl;
	explicit SurfaceCapture(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl;
};
}
