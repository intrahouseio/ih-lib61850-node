#ifndef GOOSE_SUBSCRIBER_H
#define GOOSE_SUBSCRIBER_H

#include <napi.h>
#include <goose_receiver.h>
#include <goose_subscriber.h>
#include <mms_value.h>

class NodeGooseSubscriber : public Napi::ObjectWrap<NodeGooseSubscriber> {
public:
    static void Init(Napi::Env env, Napi::Object exports);
    NodeGooseSubscriber(const Napi::CallbackInfo& info);

private:
    static Napi::FunctionReference constructor;

    Napi::Value Subscribe(const Napi::CallbackInfo& info);
    Napi::Value Stop(const Napi::CallbackInfo& info);

    static void GooseCallback(::GooseSubscriber subscriber, void* parameter);
    void ProcessGooseData(::GooseSubscriber subscriber);

    GooseReceiver receiver_;
    ::GooseSubscriber subscriber_;
    Napi::ThreadSafeFunction tsfn_;
};

#endif