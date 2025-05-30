#include "goose_subscriber.h"
#include <string>

Napi::FunctionReference NodeGooseSubscriber::constructor;

NodeGooseSubscriber::NodeGooseSubscriber(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NodeGooseSubscriber>(info) {
    receiver_ = GooseReceiver_create();
    subscriber_ = GooseSubscriber_create("GOOSE1", nullptr);
}

void NodeGooseSubscriber::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "NodeGooseSubscriber", {
        InstanceMethod("subscribe", &NodeGooseSubscriber::Subscribe),
        InstanceMethod("stop", &NodeGooseSubscriber::Stop)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("NodeGooseSubscriber", func);
}

Napi::Value NodeGooseSubscriber::Subscribe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
        Napi::TypeError::New(env, "Expected interface (string), callback (function)").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string interface = info[0].As<Napi::String>().Utf8Value();
    Napi::Function callback = info[1].As<Napi::Function>();

    tsfn_ = Napi::ThreadSafeFunction::New(env, callback, "GooseCallback", 0, 1, this);

    GooseSubscriber_setListener(subscriber_, GooseCallback, this);
    GooseReceiver_addSubscriber(receiver_, subscriber_);
    GooseReceiver_setInterfaceId(receiver_, interface.c_str());
    GooseReceiver_start(receiver_);

    return env.Null();
}

Napi::Value NodeGooseSubscriber::Stop(const Napi::CallbackInfo& info) {
    GooseReceiver_stop(receiver_);
    GooseReceiver_destroy(receiver_);
    receiver_ = GooseReceiver_create();
    subscriber_ = GooseSubscriber_create("GOOSE1", nullptr);
    tsfn_.Release();
    return info.Env().Null();
}

void NodeGooseSubscriber::GooseCallback(::GooseSubscriber subscriber, void* parameter) {
    NodeGooseSubscriber* self = static_cast<NodeGooseSubscriber*>(parameter);
    self->ProcessGooseData(subscriber);
}

void NodeGooseSubscriber::ProcessGooseData(::GooseSubscriber subscriber) {
    auto callback = [](Napi::Env env, Napi::Function jsCallback, ::GooseSubscriber subscriber) {
        MmsValue* values = GooseSubscriber_getDataSetValues(subscriber);
        Napi::Array result = Napi::Array::New(env);
        if (values && MmsValue_getType(values) == MMS_ARRAY) {
            int size = MmsValue_getArraySize(values);
            for (int i = 0; i < size; i++) {
                MmsValue* element = MmsValue_getElement(values, i);
                if (!element) continue;
                switch (MmsValue_getType(element)) {
                    case MMS_BOOLEAN:
                        result.Set(i, Napi::Boolean::New(env, MmsValue_getBoolean(element)));
                        break;
                    case MMS_FLOAT:
                        result.Set(i, Napi::Number::New(env, MmsValue_toFloat(element)));
                        break;
                    case MMS_INTEGER:
                        result.Set(i, Napi::Number::New(env, MmsValue_toInt32(element)));
                        break;
                    case MMS_VISIBLE_STRING:
                        result.Set(i, Napi::String::New(env, MmsValue_toString(element)));
                        break;
                    default:
                        result.Set(i, Napi::String::New(env, "Unsupported type"));
                }
            }
        } else {
            jsCallback.Call({Napi::String::New(env, "Invalid GOOSE data"), env.Null()});
            return;
        }
        jsCallback.Call({env.Null(), result});
    };

    tsfn_.NonBlockingCall(subscriber, callback);
}