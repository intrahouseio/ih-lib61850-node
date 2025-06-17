/*#ifndef GOOSE_SUBSCRIBER_H
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

#endif*/

#ifndef GOOSE_SUBSCRIBER_H
#define GOOSE_SUBSCRIBER_H

#include <napi.h>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <map>
#include <iec61850_client.h>
#include <cstdint>
#include <goose_receiver.h>
#include <goose_subscriber.h>
#include <mms_value.h>
#include <cmath>

struct ResultData {
    MmsType type;
    float floatValue;
    int32_t intValue;
    bool boolValue;
    std::string stringValue;
    std::vector<ResultData> structureElements;
    std::vector<ResultData> arrayElements;
    bool isValid;
    std::string errorReason;
};

class NodeGOOSESubscriber : public Napi::ObjectWrap<NodeGOOSESubscriber> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    NodeGOOSESubscriber(const Napi::CallbackInfo& info);
    ~NodeGOOSESubscriber();

private:
    static Napi::FunctionReference constructor;
    void cleanupResources(); // Объявление метода
    Napi::Value Subscribe(const Napi::CallbackInfo& info);
    Napi::Value Unsubscribe(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);

    static void GooseCallback(GooseSubscriber subscriber, void* parameter);
    ResultData ConvertMmsValue(MmsValue* val, const std::string& attrName);
    Napi::Value ToNapiValue(Napi::Env env, const ResultData& data);

    Napi::ThreadSafeFunction tsfn_;
    GooseReceiver receiver_;
    GooseSubscriber subscriber_;
    std::string goCbRef_;
    std::mutex mutex_;
    bool isSubscribed_;
    std::string interfaceId_;
};

#endif  // GOOSE_SUBSCRIBER_H