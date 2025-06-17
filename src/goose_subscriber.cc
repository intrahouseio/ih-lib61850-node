#include <napi.h>
#include "goose_subscriber.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <iostream>
#include <cmath>

Napi::FunctionReference NodeGOOSESubscriber::constructor;

Napi::Object NodeGOOSESubscriber::Init(Napi::Env env, Napi::Object exports) {
    std::cout << "[DEBUG] Initializing NodeGOOSESubscriber class\n";
    Napi::Function func = DefineClass(env, "NodeGOOSESubscriber", {
        InstanceMethod("subscribe", &NodeGOOSESubscriber::Subscribe),
        InstanceMethod("unsubscribe", &NodeGOOSESubscriber::Unsubscribe),
        InstanceMethod("getStatus", &NodeGOOSESubscriber::GetStatus)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("NodeGOOSESubscriber", func);
    return exports;
}

NodeGOOSESubscriber::NodeGOOSESubscriber(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NodeGOOSESubscriber>(info) {
    std::cout << "[DEBUG] Constructing NodeGOOSESubscriber\n";
    if (info.Length() < 1 || !info[0].IsFunction()) {
        std::cout << "[ERROR] Constructor: Expected a callback function\n";
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    tsfn_ = Napi::ThreadSafeFunction::New(
        info.Env(),
        emit,
        "NodeGOOSESubscriberTSFN",
        0,
        1,
        [](Napi::Env) { std::cout << "[DEBUG] ThreadSafeFunction finalized\n"; }
    );
    receiver_ = GooseReceiver_create();
    if (!receiver_) {
        std::cout << "[ERROR] Failed to create GooseReceiver\n";
        Napi::Error::New(info.Env(), "Failed to create GooseReceiver").ThrowAsJavaScriptException();
        return;
    }
    std::cout << "[DEBUG] GooseReceiver created\n";
    subscriber_ = nullptr;
    isSubscribed_ = false;
}

NodeGOOSESubscriber::~NodeGOOSESubscriber() {
    std::cout << "[DEBUG] Destructing NodeGOOSESubscriber\n";
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupResources();
    if (tsfn_) {
        tsfn_.Release();
        std::cout << "[DEBUG] ThreadSafeFunction released\n";
    }
}

void NodeGOOSESubscriber::cleanupResources() {
    if (isSubscribed_) {
        if (subscriber_ && receiver_) {
            GooseReceiver_removeSubscriber(receiver_, subscriber_);
            std::cout << "[DEBUG] Subscriber removed from receiver\n";
        }
        if (subscriber_) {
            GooseSubscriber_destroy(subscriber_);
            subscriber_ = nullptr;
            std::cout << "[DEBUG] GooseSubscriber destroyed\n";
        }
        if (receiver_) {
            GooseReceiver_stop(receiver_);
            GooseReceiver_destroy(receiver_);
            receiver_ = nullptr;
            std::cout << "[DEBUG] GooseReceiver stopped and destroyed\n";
        }
        isSubscribed_ = false;
    }
}

Napi::Value NodeGOOSESubscriber::Subscribe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::cout << "[DEBUG] Subscribe called\n";
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        std::cout << "[ERROR] Subscribe: Expected interfaceId (string) and goCbRef (string)\n";
        Napi::TypeError::New(env, "Expected interfaceId (string) and goCbRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string interfaceId = info[0].As<Napi::String>().Utf8Value();
    std::string goCbRef = info[1].As<Napi::String>().Utf8Value();
    std::cout << "[DEBUG] Subscribe: interfaceId=" << interfaceId << ", goCbRef=" << goCbRef << "\n";

    std::lock_guard<std::mutex> lock(mutex_);
    if (isSubscribed_) {
        std::cout << "[ERROR] Subscribe: Already subscribed\n";
        Napi::Error::New(env, "Already subscribed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    GooseReceiver_setInterfaceId(receiver_, interfaceId.c_str());
    std::cout << "[DEBUG] Set interfaceId: " << interfaceId << "\n";

    char* goCbRefBuf = new char[goCbRef.length() + 1];
    strcpy(goCbRefBuf, goCbRef.c_str());
    subscriber_ = GooseSubscriber_create(goCbRefBuf, nullptr);
    delete[] goCbRefBuf;
    if (!subscriber_) {
        std::cout << "[ERROR] Subscribe: Failed to create GooseSubscriber\n";
        Napi::Error::New(env, "Failed to create GooseSubscriber").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::cout << "[DEBUG] GooseSubscriber created with goCbRef: " << goCbRef << "\n";

    // Установка мультикастового адреса
    uint8_t dstMac[] = {0x01, 0x0C, 0xCD, 0x01, 0x00, 0x00};
    GooseSubscriber_setDstMac(subscriber_, dstMac);

    GooseSubscriber_setListener(subscriber_, GooseCallback, this);
    std::cout << "[DEBUG] GooseSubscriber listener set\n";
    GooseReceiver_addSubscriber(receiver_, subscriber_);
    std::cout << "[DEBUG] Subscriber added to receiver\n";
    GooseReceiver_start(receiver_);
    std::cout << "[DEBUG] GooseReceiver started\n";

    isSubscribed_ = true;
    goCbRef_ = goCbRef;
    interfaceId_ = interfaceId;

    tsfn_.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
        std::cout << "[DEBUG] Emitting 'subscribed' event\n";
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, "subscribed"));
        eventObj.Set("goCbRef", Napi::String::New(env, goCbRef_.c_str()));
        jsCallback.Call({Napi::String::New(env, "control"), eventObj});
    });

    return env.Undefined();
}

Napi::Value NodeGOOSESubscriber::Unsubscribe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::cout << "[DEBUG] Unsubscribe called\n";
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isSubscribed_) {
        std::cout << "[ERROR] Unsubscribe: Not subscribed\n";
        Napi::Error::New(env, "Not subscribed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    cleanupResources();

    tsfn_.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
        std::cout << "[DEBUG] Emitting 'unsubscribed' event\n";
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, "unsubscribed"));
        eventObj.Set("goCbRef", Napi::String::New(env, goCbRef_.c_str()));
        jsCallback.Call({Napi::String::New(env, "control"), eventObj});
    });

    return env.Undefined();
}

Napi::Value NodeGOOSESubscriber::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::cout << "[DEBUG] GetStatus called\n";
    std::lock_guard<std::mutex> lock(mutex_);
    Napi::Object status = Napi::Object::New(env);
    status.Set("isSubscribed", Napi::Boolean::New(env, isSubscribed_));
    status.Set("goCbRef", Napi::String::New(env, goCbRef_.c_str()));
    status.Set("interfaceId", Napi::String::New(env, interfaceId_.c_str()));
    return status;
}

ResultData NodeGOOSESubscriber::ConvertMmsValue(MmsValue* val, const std::string& attrName) {
    std::cout << "[DEBUG] ConvertMmsValue called for attrName: " << attrName << "\n";
    ResultData data = { MmsValue_getType(val), 0.0f, 0, false, "", {}, {}, true, "" };
    switch (data.type) {
        case MMS_FLOAT:
            data.floatValue = MmsValue_toFloat(val);
            if (std::isnan(data.floatValue) || std::isinf(data.floatValue)) {
                data.isValid = false;
                data.errorReason = "Invalid float value";
                std::cout << "[ERROR] ConvertMmsValue: Invalid float value\n";
            }
            break;
        case MMS_INTEGER:
            data.intValue = MmsValue_toInt32(val);
            break;
        case MMS_BOOLEAN:
            data.boolValue = MmsValue_getBoolean(val);
            break;
        case MMS_VISIBLE_STRING:
            data.stringValue = MmsValue_toString(val) ? MmsValue_toString(val) : "";
            break;
        case MMS_UTC_TIME: {
            uint64_t timestamp = MmsValue_getUtcTimeInMs(val);
            time_t time = timestamp / 1000;
            char timeStr[64];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", gmtime(&time));
            data.stringValue = std::string(timeStr) + "." + std::to_string(timestamp % 1000);
            } break;
        case MMS_BIT_STRING: {
            int bitSize = MmsValue_getBitStringSize(val);
            char bitStr[128];
            snprintf(bitStr, sizeof(bitStr), "BitString(size=%d)", bitSize);
            data.stringValue = bitStr;
            } break;
        case MMS_STRUCTURE: {
            int size = MmsValue_getArraySize(val);
            std::cout << "[DEBUG] ConvertMmsValue: Structure with size " << size << "\n";
            for (int j = 0; j < size; j++) {
                MmsValue* element = MmsValue_getElement(val, j);
                if (element) {
                    std::string subAttrName = attrName + ".field" + std::to_string(j);
                    ResultData subData = ConvertMmsValue(element, subAttrName);
                    if (subData.isValid) data.structureElements.push_back(subData);
                }
            }
            } break;
        case MMS_ARRAY: {
            int size = MmsValue_getArraySize(val);
            std::cout << "[DEBUG] ConvertMmsValue: Array with size " << size << "\n";
            for (int j = 0; j < size; j++) {
                MmsValue* element = MmsValue_getElement(val, j);
                if (element) {
                    ResultData subData = ConvertMmsValue(element, attrName + "[" + std::to_string(j) + "]");
                    data.arrayElements.push_back(subData);
                }
            }
            } break;
        case MMS_DATA_ACCESS_ERROR:
            data.isValid = false;
            data.errorReason = "Data access error";
            std::cout << "[ERROR] ConvertMmsValue: Data access error\n";
            break;
        default:
            data.isValid = false;
            data.errorReason = "Unsupported type: " + std::to_string(data.type);
            std::cout << "[ERROR] ConvertMmsValue: Unsupported type " << data.type << "\n";
    }
    return data;
}

Napi::Value NodeGOOSESubscriber::ToNapiValue(Napi::Env env, const ResultData& data) {
    std::cout << "[DEBUG] ToNapiValue called\n";
    if (!data.isValid) {
        std::cout << "[ERROR] ToNapiValue: Invalid data, error: " << data.errorReason << "\n";
        return Napi::String::New(env, data.errorReason);
    }
    switch (data.type) {
        case MMS_FLOAT: return Napi::Number::New(env, data.floatValue);
        case MMS_INTEGER: return Napi::Number::New(env, data.intValue);
        case MMS_BOOLEAN: return Napi::Boolean::New(env, data.boolValue);
        case MMS_VISIBLE_STRING: case MMS_UTC_TIME: case MMS_BIT_STRING: return Napi::String::New(env, data.stringValue);
        case MMS_STRUCTURE: {
            Napi::Object structObj = Napi::Object::New(env);
            for (size_t i = 0; i < data.structureElements.size(); i++)
                structObj.Set(Napi::String::New(env, "field" + std::to_string(i)), ToNapiValue(env, data.structureElements[i]));
            return structObj;
        }
        case MMS_ARRAY: {
            Napi::Array array = Napi::Array::New(env, data.arrayElements.size());
            for (size_t i = 0; i < data.arrayElements.size(); i++)
                array.Set(uint32_t(i), ToNapiValue(env, data.arrayElements[i]));
            return array;
        }
        default: return Napi::String::New(env, "Unsupported type");
    }
}

void NodeGOOSESubscriber::GooseCallback(GooseSubscriber subscriber, void* parameter) {
    NodeGOOSESubscriber* self = static_cast<NodeGOOSESubscriber*>(parameter);
    std::cout << "[DEBUG] GooseCallback triggered\n";
    std::lock_guard<std::mutex> lock(self->mutex_);

    const char* goCbRef = GooseSubscriber_getGoCbRef(subscriber);
    uint32_t stNum = GooseSubscriber_getStNum(subscriber);
    uint32_t sqNum = GooseSubscriber_getSqNum(subscriber);
    uint32_t confRev = GooseSubscriber_getConfRev(subscriber);
    MmsValue* values = GooseSubscriber_getDataSetValues(subscriber);

    std::cout << "[DEBUG] GooseCallback: goCbRef=" << (goCbRef ? goCbRef : "null")
              << ", stNum=" << stNum << ", sqNum=" << sqNum << ", confRev=" << confRev << "\n";
    if (!values) {
        std::cout << "[ERROR] GooseCallback: No data set values\n";
    } else if (MmsValue_getType(values) != MMS_ARRAY) {
        std::cout << "[ERROR] GooseCallback: Data set is not an MMS_ARRAY, type=" << MmsValue_getType(values) << "\n";
    }

    self->tsfn_.NonBlockingCall([goCbRef, stNum, sqNum, confRev, values, self](Napi::Env env, Napi::Function jsCallback) {
        std::cout << "[DEBUG] NonBlockingCall in GooseCallback\n";
        if (env.IsExceptionPending()) {
            std::cout << "[ERROR] NonBlockingCall: JavaScript exception pending\n";
            return;
        }
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("type", Napi::String::New(env, "data"));
        eventObj.Set("event", Napi::String::New(env, "goose"));
        eventObj.Set("goCbRef", Napi::String::New(env, goCbRef ? goCbRef : ""));
        eventObj.Set("stNum", Napi::Number::New(env, stNum));
        eventObj.Set("sqNum", Napi::Number::New(env, sqNum));
        eventObj.Set("confRev", Napi::Number::New(env, confRev));

        Napi::Array jsValues = Napi::Array::New(env);
        if (values && MmsValue_getType(values) == MMS_ARRAY) {
            int size = MmsValue_getArraySize(values);
            std::cout << "[DEBUG] Processing MMS_ARRAY with size " << size << "\n";
            jsValues = Napi::Array::New(env, size);
            for (int i = 0; i < size; i++) {
                MmsValue* element = MmsValue_getElement(values, i);
                if (element) {
                    ResultData resultData = self->ConvertMmsValue(element, "value[" + std::to_string(i) + "]");
                    jsValues.Set(i, self->ToNapiValue(env, resultData));
                } else {
                    std::cout << "[ERROR] Null element at index " << i << "\n";
                }
            }
        } else {
            std::cout << "[ERROR] Invalid or missing MMS_ARRAY\n";
        }
        eventObj.Set("values", jsValues);

        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
        std::cout << "[DEBUG] JavaScript callback invoked\n";
    });
}