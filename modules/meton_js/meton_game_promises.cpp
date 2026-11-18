#include "meton_game_binding.h"
#include "meton_game_support.generated.h"
#include "meton_game_lifecycle.generated.h"

void MetonGameBinding::stamp_promise(v8::Local<v8::Promise> p_promise, Stamp p_stamp) {
    if (promise_stamp.IsEmpty()) promise_stamp.Reset(isolate, v8::Private::New(isolate));
    auto ctx = isolate->GetCurrentContext();
    auto stamp = v8::Array::New(isolate, 2);
    (void)stamp->CreateDataProperty(ctx, 0, v8::Uint32::New(isolate, p_stamp.owner)).FromMaybe(false);
    (void)stamp->CreateDataProperty(ctx, 1, v8::BigInt::NewFromUnsigned(isolate, p_stamp.epoch)).FromMaybe(false);
    (void)p_promise->SetPrivate(ctx, promise_stamp.Get(isolate), stamp).FromMaybe(false);
}
void MetonGameBinding::weak_reaction(const v8::WeakCallbackInfo<Reaction> &p_info) {
    auto *entry = p_info.GetParameter();
    auto *binding = entry->binding;
    const uint32_t id = entry->id;
    entry->promise.Reset();
    binding->reactions.erase(id);
}
bool MetonGameBinding::promise_event(v8::PromiseHookType p_type, v8::Local<v8::Promise> p_promise, v8::Local<v8::Value> p_parent) {
    if (closed) return true;
    auto ctx = isolate->GetCurrentContext();
    if (ctx.IsEmpty()) return true;
    if (promise_stamp.IsEmpty()) promise_stamp.Reset(isolate, v8::Private::New(isolate));
    if (promise_ticket.IsEmpty()) promise_ticket.Reset(isolate, v8::Private::New(isolate));
    if (p_type == v8::PromiseHookType::kInit) {
        v8::Local<v8::Value> parent_stamp;
        // 已存在 Promise 的来源优先，重新入树后给旧 Promise 加 then 也不会刷新其资格。
        bool inherited = false;
        if (p_parent->IsPromise() && p_parent.As<v8::Promise>()->GetPrivate(ctx, promise_stamp.Get(isolate)).ToLocal(&parent_stamp) && parent_stamp->IsArray()) {
            v8::Local<v8::Value> parent_owner;
            if (parent_stamp.As<v8::Array>()->Get(ctx, 0).ToLocal(&parent_owner) && parent_owner->IsUint32() && parent_owner.As<v8::Uint32>()->Value()) {
                (void)p_promise->SetPrivate(ctx, promise_stamp.Get(isolate), parent_stamp).FromMaybe(false);
                inherited = true;
            }
        }
        if (!inherited) stamp_promise(p_promise, current);
        if (p_parent->IsPromise()) {
            // 计入等待中和尚未执行的反应；弱句柄允许不可达反应回收，不能靠 GC 清除仍可执行的额度。
            if (reactions.size() >= MetonLifecycleSupport::EVENTS_QUEUED) return false;
            const uint32_t id = next_reaction++;
            auto &entry = reactions[id];
            entry.binding = this; entry.id = id; entry.promise.Reset(isolate, p_promise);
            entry.promise.SetWeak(&entry, weak_reaction, v8::WeakCallbackType::kParameter);
            (void)p_promise->SetPrivate(ctx, promise_ticket.Get(isolate), v8::Uint32::New(isolate, id)).FromMaybe(false);
        }
    } else if (p_type == v8::PromiseHookType::kBefore) {
        continuation_stack.push_back({current, phase});
        v8::Local<v8::Value> metadata;
        if (p_promise->GetPrivate(ctx, promise_stamp.Get(isolate)).ToLocal(&metadata) && metadata->IsArray()) {
            v8::Local<v8::Value> owner, epoch;
            auto stamp = metadata.As<v8::Array>();
            if (!stamp->Get(ctx, 0).ToLocal(&owner) || !stamp->Get(ctx, 1).ToLocal(&epoch) || !owner->IsUint32() || !epoch->IsBigInt()) return false;
            current = {owner.As<v8::Uint32>()->Value(), epoch.As<v8::BigInt>()->Uint64Value()};
        } else current = {};
        phase = MetonGameSupport::P_SAFE_POINT;
        v8::Local<v8::Value> ticket;
        if (p_promise->GetPrivate(ctx, promise_ticket.Get(isolate)).ToLocal(&ticket) && ticket->IsUint32()) reactions.erase(ticket.As<v8::Uint32>()->Value());
    } else if (p_type == v8::PromiseHookType::kAfter) {
        if (!continuation_stack.empty()) {
            current = continuation_stack.back().first; phase = continuation_stack.back().second;
            continuation_stack.pop_back();
        }
    }
    return true;
}
