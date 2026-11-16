#include "surface_transport.h"
#include "scene_channel.generated.hpp"
#include <array>
#include <atomic>
#include <deque>
#include <mutex>

namespace meton::bridge {
namespace wire = meton::engine::surface;
namespace scene = meton::engine::scene_channel;
namespace {
/** 各同步端点只由自己的 I/O 线程关闭，取消线程从不关闭正在使用的句柄。 */
struct Handle {
	HANDLE value = nullptr;
	~Handle() { close(); }
	void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
}
struct SurfaceTransport::State {
	Handle commands, events, cancel, wake, done, hello_ready, document_ready, reader_thread, writer_thread, monitor_thread;
	TransportLimits limits;
	std::uint64_t generation = 0;
	std::atomic<bool> ready{false}, reader_done{false}, writer_done{false};
	std::atomic<ULONGLONG> read_deadline{0}, write_deadline{0};
	mutable std::mutex mutex;
	TransportStatus facts;
	std::deque<Packet> reads;
	std::deque<SceneDocumentRequest> documents;
	std::deque<std::vector<std::uint8_t>> writes;
	std::string hello_body;
	bool hello_received = false, hello_replied = false;

	bool stopping() const { return WaitForSingleObject(cancel.value, 0) == WAIT_OBJECT_0; }
	void stop() {
		{ std::lock_guard<std::mutex> lock(mutex); facts.stopping = true; }
		SetEvent(cancel.value); SetEvent(wake.value);
	}
	void fail(TransportFailure failure, DWORD error) {
		{ std::lock_guard<std::mutex> lock(mutex);
			if (facts.failure == TransportFailure::none) { facts.failure = failure; facts.native_error = error; }
		}
		stop();
	}
	bool read_exact(void *bytes, DWORD size) {
		DWORD offset = 0;
		while (offset < size) {
			if (stopping()) return false;
			DWORD count = 0;
			const BOOL ok = ReadFile(commands.value, static_cast<std::uint8_t *>(bytes) + offset, size - offset, &count, nullptr);
			if (!ok || !count) {
				if (!stopping()) fail(ok ? TransportFailure::peer_closed : TransportFailure::read, ok ? ERROR_BROKEN_PIPE : GetLastError());
				return false;
			}
			offset += count;
		}
		return !stopping();
	}
	/** 两种协议共用同一读线程，正文只在核对封印开关、方向、长度和本代身份后分配。 */
	bool read_hello(std::uint8_t *bytes) {
		const auto size = scene::hello_size(bytes, scene::Kind::hello_request);
		if (!limits.allow_scene_hello || !size || hello_received) { fail(TransportFailure::shape, ERROR_INVALID_DATA); return false; }
		if (!read_exact(bytes + 16, static_cast<DWORD>(scene::header_bytes - 16))) return false;
		if (!scene::hello_identity(bytes, generation)) { fail(TransportFailure::binding, ERROR_INVALID_DATA); return false; }
		std::string body(size, '\0');
		if (!read_exact(reinterpret_cast<std::uint8_t *>(body.data()), static_cast<DWORD>(size))) return false;
		if (GetTickCount64() >= read_deadline.load()) { fail(TransportFailure::deadline, ERROR_TIMEOUT); return false; }
		{ std::lock_guard<std::mutex> lock(mutex);
			if (facts.stopping) return false;
			hello_received = true; hello_body = std::move(body); ++facts.received;
		}
		SetEvent(hello_ready.value);
		read_deadline.store(0);
		return true;
	}
	bool read_document(std::uint8_t *header) {
		const auto size = scene::bound_size(header, limits.max_document_bytes);
		bool admitted = false;
		{ std::lock_guard<std::mutex> lock(mutex); admitted = hello_replied && documents.size() < limits.max_document_requests; }
		if (!admitted || !size) { fail(TransportFailure::shape, ERROR_INVALID_DATA); return false; }
		if (!read_exact(header + 16, static_cast<DWORD>(scene::header_bytes - 16 + 2))) return false;
		SceneDocumentRequest request;
		request.lane = wire::read_le(header + 24,8); request.sequence = wire::read_le(header + 32,8);
		const auto route_bytes = wire::read_le(header + scene::header_bytes,2);
		if (!scene::data_identity(header,generation,request.lane,request.sequence) || !route_bytes || route_bytes > scene::route_max_bytes ||
				2 + route_bytes >= size || size - 2 - route_bytes > limits.max_document_bytes) { fail(TransportFailure::binding, ERROR_INVALID_DATA); return false; }
		request.route.resize(route_bytes); request.body.resize(size - 2 - route_bytes);
		if (!read_exact(request.route.data(), static_cast<DWORD>(request.route.size())) || !read_exact(request.body.data(),static_cast<DWORD>(request.body.size()))) return false;
		if (GetTickCount64() >= read_deadline.load()) { fail(TransportFailure::deadline, ERROR_TIMEOUT); return false; }
		{ std::lock_guard<std::mutex> lock(mutex);
			if (facts.stopping) return false;
			documents.push_back(std::move(request)); ++facts.received;
			SetEvent(document_ready.value);
		}
		read_deadline.store(0);
		return true;
	}
	void read_packets() {
		while (!stopping()) {
			std::array<std::uint8_t, 104> bytes{};
			// 空闲不超时；收到首字节后，头部和正文共用一个固定期限。
			if (!read_exact(bytes.data(), 1)) break;
			read_deadline.store(GetTickCount64() + limits.operation_timeout_ms);
			if (!read_exact(bytes.data() + 1, 15)) break;
			if (wire::read_le(bytes.data(), 4) == scene::magic) {
				const bool document = wire::read_le(bytes.data()+8,2) == static_cast<std::uint16_t>(scene::Kind::bound_request);
				if (!(document ? read_document(bytes.data()) : read_hello(bytes.data()))) break;
				continue;
			}
			const auto size = wire::read_le(bytes.data() + 12, 4);
			const auto kind = static_cast<wire::Kind>(wire::read_le(bytes.data() + 8, 2));
			if (wire::read_le(bytes.data(), 4) != 1179866189ULL || wire::read_le(bytes.data() + 4, 2) != 1 || wire::read_le(bytes.data() + 6, 2) != 0 ||
				wire::read_le(bytes.data() + 10, 2) != 0 || size < 48 || size > bytes.size() || wire::packet_size(kind) != size) { fail(TransportFailure::shape, ERROR_INVALID_DATA); break; }
			if (!read_exact(bytes.data() + 16, static_cast<DWORD>(size - 16))) break;
			if (GetTickCount64() >= read_deadline.load()) { fail(TransportFailure::deadline, ERROR_TIMEOUT); break; }
			Packet packet{};
			if (wire::decode(bytes.data(), static_cast<std::size_t>(size), packet) != wire::Error::ok) { fail(TransportFailure::shape, ERROR_INVALID_DATA); break; }
			if (packet.instance_generation != generation) { fail(TransportFailure::binding, ERROR_INVALID_DATA); break; }
			if (packet.kind != wire::Kind::offer_ack && packet.kind != wire::Kind::release_ack && packet.kind != wire::Kind::view_state &&
					packet.kind != wire::Kind::view_extent && packet.kind != wire::Kind::view_input) { fail(TransportFailure::direction, ERROR_INVALID_DATA); break; }
			read_deadline.store(0);
			bool full = false;
			{ std::lock_guard<std::mutex> lock(mutex); full = reads.size() >= limits.max_queue_messages;
				if (!facts.stopping && !full) { reads.push_back(packet); ++facts.received; }
			}
			if (full) { fail(TransportFailure::queue_full, ERROR_NOT_ENOUGH_QUOTA); break; }
		}
		commands.close(); reader_done.store(true);
	}
	void write_packets() {
		const HANDLE signals[]{cancel.value, wake.value};
		while (!stopping()) {
			const DWORD waited = WaitForMultipleObjects(2, signals, FALSE, INFINITE);
			if (waited != WAIT_OBJECT_0 + 1) {
				if (waited != WAIT_OBJECT_0) fail(TransportFailure::write, waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_STATE);
				break;
			}
			std::vector<std::uint8_t> bytes;
			{ std::lock_guard<std::mutex> lock(mutex);
				if (writes.empty()) { ResetEvent(wake.value); continue; }
				bytes = std::move(writes.front()); writes.pop_front();
			}
			write_deadline.store(GetTickCount64() + limits.operation_timeout_ms);
			DWORD offset = 0;
			while (offset < bytes.size() && !stopping()) {
				DWORD count = 0;
				const BOOL ok = WriteFile(events.value, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr);
				if (!ok || !count) {
					if (!stopping()) fail(TransportFailure::write, ok ? ERROR_BROKEN_PIPE : GetLastError());
					break;
				}
				offset += count;
			}
			// 同步 I/O 恰好在监视线程两次轮询之间完成，也不能把已经超过期限的写入当及时交付。
			if (offset == bytes.size() && GetTickCount64() >= write_deadline.load()) fail(TransportFailure::deadline, ERROR_TIMEOUT);
			write_deadline.store(0);
			if (offset == bytes.size()) { std::lock_guard<std::mutex> lock(mutex); ++facts.written; }
		}
		events.close(); writer_done.store(true);
	}
	void monitor() {
		while (!ready.load()) Sleep(1);
		while (!reader_done.load() || !writer_done.load()) {
			const auto now = GetTickCount64(), reading = read_deadline.load(), writing = write_deadline.load();
			if (!stopping() && ((reading && now >= reading) || (writing && now >= writing))) fail(TransportFailure::deadline, ERROR_TIMEOUT);
			if (stopping()) {
				// ERROR_NOT_FOUND 仅表示此刻没有挂起调用；继续核对线程，覆盖随后刚进入 I/O 的竞态。
				if (!reader_done.load() && reader_thread.value) CancelSynchronousIo(reader_thread.value);
				if (!writer_done.load() && writer_thread.value) CancelSynchronousIo(writer_thread.value);
			}
			Sleep(8);
		}
		{ std::lock_guard<std::mutex> lock(mutex); facts.stopping = true; facts.stopped = true; writes.clear(); }
		SetEvent(done.value);
	}
	/** 每条线程自行持有状态；外层对象析构不回收尚未完成的 I/O 栈缓冲。 */
	static DWORD WINAPI reader(void *data) {
		std::unique_ptr<std::shared_ptr<State>> owner(static_cast<std::shared_ptr<State> *>(data)); (*owner)->read_packets(); return 0;
	}
	static DWORD WINAPI writer(void *data) {
		std::unique_ptr<std::shared_ptr<State>> owner(static_cast<std::shared_ptr<State> *>(data)); (*owner)->write_packets(); return 0;
	}
	static DWORD WINAPI watcher(void *data) {
		std::unique_ptr<std::shared_ptr<State>> owner(static_cast<std::shared_ptr<State> *>(data)); (*owner)->monitor(); return 0;
	}
	static HANDLE thread(const std::shared_ptr<State> &state, LPTHREAD_START_ROUTINE action) {
		auto *owner = new std::shared_ptr<State>(state);
		const HANDLE value = CreateThread(nullptr, 0, action, owner, 0, nullptr);
		if (!value) delete owner;
		return value;
	}
};

SurfaceTransport::SurfaceTransport(std::shared_ptr<State> value) : state(std::move(value)) {}
SurfaceTransport::~SurfaceTransport() { stop(); }
std::shared_ptr<SurfaceTransport> SurfaceTransport::open(HANDLE commands, HANDLE events, std::uint64_t generation, const TransportLimits &limits) {
	auto state = std::make_shared<State>(); state->commands.value = commands; state->events.value = events;
	if (commands == events) { state->events.value = nullptr; return {}; }
	if (!generation || !limits.max_queue_messages || limits.max_queue_messages > 4096 || !limits.operation_timeout_ms ||
		GetFileType(commands) != FILE_TYPE_PIPE || GetFileType(events) != FILE_TYPE_PIPE) return {};
	state->generation = generation; state->limits = limits;
	state->cancel.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); state->wake.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	state->done.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (limits.allow_scene_hello) {
		state->hello_ready.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!state->hello_ready.value) return {};
	}
	if (limits.max_document_bytes) {
		state->document_ready.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!state->document_ready.value) return {};
	}
	if (!state->cancel.value || !state->wake.value || !state->done.value) return {};
	state->monitor_thread.value = State::thread(state, State::watcher);
	if (!state->monitor_thread.value) return {};
	state->reader_thread.value = State::thread(state, State::reader);
	state->writer_thread.value = State::thread(state, State::writer);
	if (!state->reader_thread.value) { state->commands.close(); state->reader_done.store(true); }
	if (!state->writer_thread.value) { state->events.close(); state->writer_done.store(true); }
	const bool started = state->reader_thread.value && state->writer_thread.value;
	if (!started) state->fail(TransportFailure::thread, ERROR_NOT_ENOUGH_MEMORY);
	state->ready.store(true);
	if (!started) return {};
	return std::shared_ptr<SurfaceTransport>(new SurfaceTransport(std::move(state)));
}
bool SurfaceTransport::send(const Packet &packet) {
	if (packet.instance_generation != state->generation || (packet.kind != wire::Kind::offer && packet.kind != wire::Kind::frame &&
			packet.kind != wire::Kind::extent_frame && packet.kind != wire::Kind::release && packet.kind != wire::Kind::input_ack)) return false;
	std::vector<std::uint8_t> bytes(wire::packet_size(packet.kind)); std::size_t written = 0;
	if (wire::encode(packet, bytes.data(), bytes.size(), written) != wire::Error::ok || written != bytes.size()) return false;
	{ std::lock_guard<std::mutex> lock(state->mutex);
		if (state->facts.stopping || state->writes.size() >= state->limits.max_queue_messages) return false;
		state->writes.push_back(std::move(bytes));
	}
	SetEvent(state->wake.value); return true;
}
std::vector<SurfaceTransport::Packet> SurfaceTransport::take() {
	std::lock_guard<std::mutex> lock(state->mutex); std::vector<Packet> result(state->reads.begin(), state->reads.end()); state->reads.clear(); return result;
}
std::string SurfaceTransport::take_hello() {
	std::lock_guard<std::mutex> lock(state->mutex);
	std::string result; result.swap(state->hello_body); return result;
}
bool SurfaceTransport::take_document(SceneDocumentRequest &request) {
	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->facts.stopping || state->documents.empty()) return false;
	request = std::move(state->documents.front()); state->documents.pop_front();
	if (state->documents.empty()) ResetEvent(state->document_ready.value);
	return true;
}
bool SurfaceTransport::wait_document(SceneDocumentRequest &request, DWORD timeout_ms) {
	if (!state->document_ready.value || !timeout_ms) return false;
	const HANDLE signals[]{state->cancel.value, state->document_ready.value};
	return WaitForMultipleObjects(2, signals, FALSE, timeout_ms) == WAIT_OBJECT_0 + 1 && take_document(request);
}
bool SurfaceTransport::reply_document(const SceneDocumentRequest &request, const std::string &body) {
	if (body.size() > state->limits.max_document_bytes) return false;
	auto bytes = scene::data(scene::Kind::data_reply, state->generation, request.lane, request.sequence, body);
	if (bytes.empty()) return false;
	{ std::lock_guard<std::mutex> lock(state->mutex);
		if (state->facts.stopping || state->writes.size() >= state->limits.max_queue_messages) return false;
		state->writes.push_back(std::move(bytes));
	}
	SetEvent(state->wake.value); return true;
}
std::string SurfaceTransport::wait_hello(DWORD timeout_ms) {
	if (!state->limits.allow_scene_hello || !timeout_ms) return {};
	const HANDLE signals[]{state->cancel.value, state->hello_ready.value};
	if (WaitForMultipleObjects(2, signals, FALSE, timeout_ms) != WAIT_OBJECT_0 + 1) return {};
	return take_hello();
}
bool SurfaceTransport::reply_hello(const std::string &body) {
	auto bytes = scene::hello(scene::Kind::hello_reply, state->generation, body);
	if (bytes.empty()) return false;
	{ std::lock_guard<std::mutex> lock(state->mutex);
		if (state->facts.stopping || !state->hello_received || state->hello_replied || !state->hello_body.empty() ||
				state->writes.size() >= state->limits.max_queue_messages) return false;
		state->hello_replied = true; state->writes.push_back(std::move(bytes));
	}
	SetEvent(state->wake.value); return true;
}
TransportStatus SurfaceTransport::status() const {
	std::lock_guard<std::mutex> lock(state->mutex); auto result = state->facts;
	result.queued_reads = state->reads.size(); result.queued_writes = state->writes.size(); return result;
}
void SurfaceTransport::stop() { state->stop(); }
bool SurfaceTransport::wait_stopped(DWORD timeout_ms) const { return WaitForSingleObject(state->done.value, timeout_ms) == WAIT_OBJECT_0; }
}
