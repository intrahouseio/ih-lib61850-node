#include "mms_client.h"
#include <cmath>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <functional>
#include <ctime>
#include <cinttypes>
#include <cstdint>  // Для int64_t

Napi::FunctionReference MmsClient::constructor;

struct ConnectionHandlerContext {
    MmsClient* client;
    std::mutex* mutex;
};

Napi::Object MmsClient::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "MmsClient", {
        InstanceMethod("connect", &MmsClient::Connect),
        InstanceMethod("readData", &MmsClient::ReadData),        
        InstanceMethod("controlObject", &MmsClient::ControlObject),
        InstanceMethod("close", &MmsClient::Close),
        InstanceMethod("getStatus", &MmsClient::GetStatus),
        InstanceMethod("getLogicalDevices", &MmsClient::GetLogicalDevices),
        InstanceMethod("readDataSetValues", &MmsClient::ReadDataSetValues),
        InstanceMethod("createDataSet", &MmsClient::CreateDataSet),
        InstanceMethod("deleteDataSet", &MmsClient::DeleteDataSet),
        InstanceMethod("getDataSetDirectory", &MmsClient::GetDataSetDirectory),
        InstanceMethod("browseDataModel", &MmsClient::BrowseDataModel),
        InstanceMethod("enableReporting", &MmsClient::EnableReporting),
        InstanceMethod("disableReporting", &MmsClient::DisableReporting)          
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("MmsClient", func);
    return exports;
}

// Конвертация MmsValue → MmsClient::ResultData
static MmsClient::ResultData ConvertMmsValueToResultData(MmsValue* val, const std::string& attrName) {
    MmsClient::ResultData data;
    if (!val) {
        data.type = MMS_DATA_ACCESS_ERROR;
        data.isValid = false;
        data.errorReason = "Null value";
        return data;
    }

    data.type = MmsValue_getType(val);
    data.isValid = true;
    data.errorReason = "";

    switch (data.type) {
        case MMS_FLOAT:
            data.floatValue = MmsValue_toFloat(val);
            if (std::isnan(data.floatValue) || std::isinf(data.floatValue)) {
                data.isValid = false;
                data.errorReason = "Invalid float";
            }
            break;

        case MMS_INTEGER:
        case MMS_UNSIGNED:
            data.intValue = MmsValue_toInt64(val);
            if (attrName == "ctlModel") {
                switch (data.intValue) {
                    case 0: data.stringValue = "status-only"; break;
                    case 1: data.stringValue = "direct-with-normal-security"; break;
                    case 2: data.stringValue = "sbo-with-normal-security"; break;
                    case 3: data.stringValue = "direct-with-enhanced-security"; break;
                    case 4: data.stringValue = "sbo-with-enhanced-security"; break;
                    default: data.stringValue = "unknown(" + std::to_string(data.intValue) + ")";
                }
            }
            break;

        case MMS_BOOLEAN:
            data.boolValue = MmsValue_getBoolean(val);
            break;

        case MMS_VISIBLE_STRING:
            {
                const char* str = MmsValue_toString(val);
                data.stringValue = str ? str : "";
            }
            break;

        case MMS_UTC_TIME:
            {
                uint64_t ms = MmsValue_getUtcTimeInMs(val);
                time_t sec = ms / 1000;
                char buf[64];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&sec));
                data.stringValue = std::string(buf) + "." + std::to_string(ms % 1000);
            }
            break;

        case MMS_BIT_STRING:
            {
                int size = MmsValue_getBitStringSize(val);
                uint64_t bits = MmsValue_getBitStringAsInteger(val);
                data.intValue = static_cast<int64_t>(bits);

                if (attrName == "q") {
                    std::string q;
                    if (bits & QUALITY_VALIDITY_INVALID) q += "Invalid|";
                    if (bits & QUALITY_VALIDITY_QUESTIONABLE) q += "Questionable|";
                    if (q.empty()) q = "Good";
                    else if (!q.empty()) q.pop_back();
                    data.stringValue = q;
                }
                else if (size == 2 && attrName.find("stVal") != std::string::npos) {
                    switch (bits) {
                        case 0: data.stringValue = "intermediate-state"; break;
                        case 1: data.stringValue = "off"; break;
                        case 2: data.stringValue = "on"; break;
                        case 3: data.stringValue = "bad-state"; break;
                        default: data.stringValue = "unknown(" + std::to_string(bits) + ")";
                    }
                }
            }
            break;

        case MMS_STRUCTURE:
            {
                int size = MmsValue_getArraySize(val);
                for (int i = 0; i < size; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        data.structureElements.push_back(ConvertMmsValueToResultData(el, attrName + "." + std::to_string(i)));
                    }
                }
            }
            break;

        case MMS_ARRAY:
            {
                int size = MmsValue_getArraySize(val);
                for (int i = 0; i < size; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        data.arrayElements.push_back(ConvertMmsValueToResultData(el, attrName + "[" + std::to_string(i) + "]"));
                    }
                }
            }
            break;

        default:
            data.isValid = false;
            data.errorReason = "Unsupported MMS type: " + std::to_string(data.type);
            break;
    }
    return data;
}

// ResultData → Napi::Value
static Napi::Value ResultDataToNapi(Napi::Env env, const MmsClient::ResultData& data) {
    if (!data.isValid) {
        return Napi::String::New(env, data.errorReason);
    }

    switch (data.type) {
        case MMS_FLOAT: return Napi::Number::New(env, data.floatValue);
        case MMS_INTEGER:
        case MMS_UNSIGNED:
            if (!data.stringValue.empty()) return Napi::String::New(env, data.stringValue);
            return Napi::Number::New(env, static_cast<double>(data.intValue));
        case MMS_BOOLEAN: return Napi::Boolean::New(env, data.boolValue);
        case MMS_VISIBLE_STRING:
        case MMS_UTC_TIME: return Napi::String::New(env, data.stringValue);
        case MMS_BIT_STRING:
            if (!data.stringValue.empty()) return Napi::String::New(env, data.stringValue);
            return Napi::Number::New(env, static_cast<double>(data.intValue));
        case MMS_STRUCTURE: {
            Napi::Object obj = Napi::Object::New(env);
            for (size_t i = 0; i < data.structureElements.size(); ++i) {
                obj.Set(std::to_string(i), ResultDataToNapi(env, data.structureElements[i]));
            }
            return obj;
        }
        case MMS_ARRAY: {
            Napi::Array arr = Napi::Array::New(env, data.arrayElements.size());
            for (size_t i = 0; i < data.arrayElements.size(); ++i) {
                arr.Set(i, ResultDataToNapi(env, data.arrayElements[i]));
            }
            return arr;
        }
        default:
            return Napi::String::New(env, "Unsupported type");
    }
}

// ResultData → std::string (для GetLogicalDevices)
static std::string ResultDataToString(const MmsClient::ResultData& data) {
    if (!data.isValid) return data.errorReason;
    switch (data.type) {
        case MMS_FLOAT: return std::to_string(data.floatValue);
        case MMS_INTEGER:
        case MMS_UNSIGNED: return data.stringValue.empty() ? std::to_string(data.intValue) : data.stringValue;
        case MMS_BOOLEAN: return data.boolValue ? "true" : "false";
        case MMS_VISIBLE_STRING:
        case MMS_UTC_TIME: return data.stringValue;
        case MMS_BIT_STRING: return data.stringValue.empty() ? std::to_string(data.intValue) : data.stringValue;
        case MMS_STRUCTURE:
        case MMS_ARRAY: return "complex";
        default: return "type_" + std::to_string(data.type);
    }
}

MmsClient::MmsClient(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MmsClient>(info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    connection_ = IedConnection_create();
    running_ = false;
    connected_ = false;
    clientID_ = "mms_client";
    usingPrimaryIp_ = true;
    try {
        tsfn_ = Napi::ThreadSafeFunction::New(
            info.Env(),
            emit,
            "MmsClientTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), std::string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

MmsClient::~MmsClient() {
    std::lock_guard<std::mutex> lock(connMutex_);
    if (running_) {
        running_ = false;
        if (connected_) {
            printf("Destructor closing connection, clientID: %s\n", clientID_.c_str());
            IedConnection_close(connection_);
            connected_ = false;
        }
    }

    // Cleanup active reports
    for (auto& [rcbRef, reportInfo] : activeReports_) {
        printf("Cleaning up report for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
        if (reportInfo.rcb) {
            ClientReportControlBlock_setRptEna(reportInfo.rcb, false);
            IedClientError error;
            IedConnection_setRCBValues(connection_, &error, reportInfo.rcb, RCB_ELEMENT_RPT_ENA, true);
            ClientReportControlBlock_destroy(reportInfo.rcb);
        }
        if (reportInfo.dataSet) {
            ClientDataSet_destroy(reportInfo.dataSet);
        }
        if (reportInfo.dataSetDirectory) {
            LinkedList_destroy(reportInfo.dataSetDirectory);
        }
    }
    activeReports_.clear();

    if (thread_.joinable()) {
        thread_.join();
    }

    if (connection_) {
        IedConnection_destroy(connection_);
        connection_ = nullptr;
    }
    if (tsfn_) {
        tsfn_.Release();
        tsfn_ = Napi::ThreadSafeFunction();
    }
}

void MmsClient::ConnectionHandler(void* parameter, IedConnection connection, IedConnectionState newState) {
    ConnectionHandlerContext* context = static_cast<ConnectionHandlerContext*>(parameter);
    MmsClient* client = context->client;
    std::mutex* mutex = context->mutex;

    std::string stateStr;
    bool isConnected = false;
    switch (newState) {
        case IED_STATE_CLOSED:
            stateStr = "closed";
            isConnected = false;
            break;
        case IED_STATE_CONNECTING:
            stateStr = "connecting";
            isConnected = false;
            break;
        case IED_STATE_CONNECTED:
            stateStr = "connected";
            isConnected = true;
            break;
        case IED_STATE_CLOSING:
            stateStr = "closing";
            isConnected = false;
            break;
        default:
            stateStr = "unknown";
            isConnected = false;
    }

    {
        std::lock_guard<std::mutex> lock(*mutex);
        client->connected_ = isConnected;
    }

    printf("Connection state changed to %s, clientID: %s\n", stateStr.c_str(), client->clientID_.c_str());
    client->tsfn_.NonBlockingCall([client, stateStr, isConnected](Napi::Env env, Napi::Function jsCallback) {
        try {
            if (env.IsExceptionPending()) {
                printf("ConnectionHandler: Exception pending in env, clientID: %s\n", client->clientID_.c_str());
                return;
            }
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, client->clientID_.c_str()));
            eventObj.Set("type", Napi::String::New(env, "control"));
            eventObj.Set("event", Napi::String::New(env, "stateChanged"));
            eventObj.Set("state", Napi::String::New(env, stateStr));
            eventObj.Set("isConnected", Napi::Boolean::New(env, isConnected));
            std::vector<napi_value> args = {Napi::String::New(env, "conn"), eventObj};
            jsCallback.Call(args);
        } catch (const Napi::Error& e) {
            printf("N-API Callback Error in ConnectionHandler: %s, clientID: %s\n", e.Message().c_str(), client->clientID_.c_str());
        }
    });
}

Napi::Value MmsClient::Connect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Expected an object with connection parameters").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object params = info[0].As<Napi::Object>();

    if (!params.Has("ip") || !params.Get("ip").IsString() ||
        !params.Has("port") || !params.Get("port").IsNumber() ||
        !params.Has("clientID") || !params.Get("clientID").IsString()) {
        Napi::TypeError::New(env, "Object must contain 'ip' (string), 'port' (number), and 'clientID' (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string ip = params.Get("ip").As<Napi::String>().Utf8Value();
    int port = params.Get("port").As<Napi::Number>().Int32Value();
    clientID_ = params.Get("clientID").As<Napi::String>().Utf8Value();
    std::string ipReserve = "";
    if (params.Has("ipReserve") && params.Get("ipReserve").IsString()) {
        ipReserve = params.Get("ipReserve").As<Napi::String>().Utf8Value();
    }
    int reconnectDelay = 5;
    if (params.Has("reconnectDelay") && params.Get("reconnectDelay").IsNumber()) {
        reconnectDelay = params.Get("reconnectDelay").As<Napi::Number>().Int32Value();
    }

    if (ip.empty() || port <= 0 || clientID_.empty()) {
        Napi::Error::New(env, "Invalid 'ip', 'port', or 'clientID'").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    {
        std::lock_guard<std::mutex> lock(connMutex_);
        if (running_) {
            Napi::Error::New(env, "Client already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    try {
        printf("Creating connection to %s:%d, clientID: %s\n", ip.c_str(), port, clientID_.c_str());
        running_ = true;
        usingPrimaryIp_ = true;

        ConnectionHandlerContext* context = new ConnectionHandlerContext{this, &connMutex_};
        IedConnection_installStateChangedHandler(connection_, ConnectionHandler, context);

        thread_ = std::thread([this, ip, ipReserve, port, reconnectDelay, context]() {
            int primaryRetryCount = 0;
            int reserveRetryCount = 0;
            const int maxRetries = 3;
            std::string currentIp = ip;
            bool isPrimary = true;

            while (running_) {
                printf("Attempting to connect to %s:%d (attempt %d/%d), clientID: %s\n",
                       currentIp.c_str(), port, (isPrimary ? primaryRetryCount : reserveRetryCount) + 1, maxRetries, clientID_.c_str());
                IedClientError error;
                IedConnection_connect(connection_, &error, currentIp.c_str(), port);

                {
                    std::lock_guard<std::mutex> lock(connMutex_);
                    connected_ = (error == IED_ERROR_OK);
                    usingPrimaryIp_ = isPrimary;
                }

                if (connected_) {
                    printf("Connected successfully to %s:%d, clientID: %s\n", currentIp.c_str(), port, clientID_.c_str());
                    primaryRetryCount = 0;
                    reserveRetryCount = 0;
                    tsfn_.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
                        try {
                            if (env.IsExceptionPending()) {
                                printf("Connect: Exception pending in env, clientID: %s\n", clientID_.c_str());
                                return;
                            }
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "opened"));
                            eventObj.Set("reason", Napi::String::New(env, "connection established"));
                            eventObj.Set("isPrimaryIP", Napi::Boolean::New(env, usingPrimaryIp_));
                            jsCallback.Call({Napi::String::New(env, "conn"), eventObj});
                        } catch (const Napi::Error& e) {
                            printf("N-API Callback Error in Connect: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                        }
                    });

                    while (running_) {
                        {
                            std::lock_guard<std::mutex> lock(connMutex_);
                            if (!connected_ || !running_) break;
                        }

                        if (!isPrimary && !ipReserve.empty()) {
                            IedConnection testConn = IedConnection_create();
                            IedClientError testError;
                            IedConnection_connect(testConn, &testError, ip.c_str(), port);
                            if (testError == IED_ERROR_OK) {
                                IedConnection_close(testConn);
                                IedConnection_destroy(testConn);
                                printf("Switching back to primary IP %s, clientID: %s\n", ip.c_str(), clientID_.c_str());
                                currentIp = ip;
                                isPrimary = true;
                                IedConnection_close(connection_);
                                {
                                    std::lock_guard<std::mutex> lock(connMutex_);
                                    connected_ = false;
                                }
                                break;
                            }
                            IedConnection_destroy(testConn);
                        }

                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                } else {
                    printf("Connection failed to %s:%d, error: %d, clientID: %s\n", currentIp.c_str(), port, error, clientID_.c_str());
                    tsfn_.NonBlockingCall([this, currentIp, isPrimary, retryCount = (isPrimary ? primaryRetryCount : reserveRetryCount)](Napi::Env env, Napi::Function jsCallback) {
                        try {
                            if (env.IsExceptionPending()) {
                                printf("Connect: Exception pending in env, clientID: %s\n", clientID_.c_str());
                                return;
                            }
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "reconnecting"));
                            eventObj.Set("reason", Napi::String::New(env, std::string("attempt ") + std::to_string(retryCount + 1) + " to " + currentIp));
                            eventObj.Set("isPrimaryIP", Napi::Boolean::New(env, isPrimary));
                            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        } catch (const Napi::Error& e) {
                            printf("N-API Callback Error in Connect: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                        }
                    });

                    if (isPrimary) {
                        primaryRetryCount++;
                    } else {
                        reserveRetryCount++;
                    }

                    if (isPrimary && primaryRetryCount >= maxRetries && !ipReserve.empty()) {
                        printf("Primary IP %s unresponsive after %d attempts, switching to reserve IP %s, clientID: %s\n",
                               ip.c_str(), maxRetries, ipReserve.c_str(), clientID_.c_str());
                        currentIp = ipReserve;
                        isPrimary = false;
                        primaryRetryCount = 0;
                        reserveRetryCount = 0;
                    } else if (!isPrimary && reserveRetryCount >= maxRetries) {
                        printf("Reserve IP %s unresponsive after %d attempts, switching back to primary IP %s, clientID: %s\n",
                               ipReserve.c_str(), maxRetries, ip.c_str(), clientID_.c_str());
                        currentIp = ip;
                        isPrimary = true;
                        reserveRetryCount = 0;
                        primaryRetryCount = 0;
                    }

                    printf("Reconnection attempt failed, retrying in %d seconds, clientID: %s\n", reconnectDelay, clientID_.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(reconnectDelay));
                }

                if (!running_ && connected_) {
                    printf("Thread stopped by client, closing connection, clientID: %s\n", clientID_.c_str());
                    IedConnection_close(connection_);
                    {
                        std::lock_guard<std::mutex> lock(connMutex_);
                        connected_ = false;
                    }
                    delete context;
                    return;
                }
            }
            delete context;
        });

        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in Connect: %s, clientID: %s\n", e.what(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                if (env.IsExceptionPending()) {
                    printf("Connect: Exception pending in env, clientID: %s\n", clientID_.c_str());
                    return;
                }
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Thread exception: ") + e.what()));
                std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                jsCallback.Call(args);
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in Connect: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        running_ = false;
        return env.Undefined();
    }
}

Napi::Value MmsValueToNapi(Napi::Env env, MmsValue* value) {
    if (!value) return env.Null();
    MmsClient::ResultData data = ConvertMmsValueToResultData(value, "");
    return ResultDataToNapi(env, data);
}

Napi::Value MmsClient::ReadDataSetValues(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!connected_) {
        Napi::Error::New(env, "Client not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    // === Парсим аргументы ===
    std::vector<std::string> datasetRefs;

    if (info.Length() != 1) {
        Napi::TypeError::New(env, "Expected one argument: string or array of strings").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info[0].IsString()) {
        datasetRefs.push_back(info[0].As<Napi::String>().Utf8Value());
    } else if (info[0].IsArray()) {
        Napi::Array arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            auto val = arr.Get(i);
            if (val.IsString()) {
                datasetRefs.push_back(val.As<Napi::String>().Utf8Value());
            } else {
                Napi::TypeError::New(env, "All array elements must be strings").ThrowAsJavaScriptException();
                return env.Null();
            }
        }
    } else {
        Napi::TypeError::New(env, "Argument must be string or array of strings").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (datasetRefs.empty()) {
        return Napi::Array::New(env, 0);
    }

    // === Основная работа ===
    std::lock_guard<std::mutex> lock(connMutex_);
    Napi::Array jsResults = Napi::Array::New(env, datasetRefs.size());

    for (size_t i = 0; i < datasetRefs.size(); ++i) {
        const std::string& datasetRef = datasetRefs[i];
        Napi::Object result = Napi::Object::New(env);
        result.Set("datasetRef", Napi::String::New(env, datasetRef));

        IedClientError error;
        bool isDeletable = false;

        // 1. Получаем список членов DataSet (их полные имена)
        LinkedList members = IedConnection_getDataSetDirectory(connection_, &error, datasetRef.c_str(), &isDeletable);
        if (error != IED_ERROR_OK || !members) {
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "Failed to get dataset directory: error " + std::to_string(error)));
            jsResults.Set(i, result);
            continue;
        }

        // 2. Читаем значения DataSet
        ClientDataSet clientDataSet = IedConnection_readDataSetValues(connection_, &error, datasetRef.c_str(), nullptr);
        if (error != IED_ERROR_OK || !clientDataSet) {
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "Failed to read dataset values: error " + std::to_string(error)));
            LinkedList_destroy(members);
            jsResults.Set(i, result);
            continue;
        }

        MmsValue* values = ClientDataSet_getValues(clientDataSet);
        if (!values || MmsValue_getType(values) != MMS_ARRAY) {
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "DataSet has invalid or empty values"));
            ClientDataSet_destroy(clientDataSet);
            LinkedList_destroy(members);
            jsResults.Set(i, result);
            continue;
        }

        // 3. Формируем объект: { "LD/LN.DO.DA": value }
        Napi::Object valuesObj = Napi::Object::New(env);
        LinkedList member = members;
        int index = 0;

        while (member) {
            if (member->data) {
                char* memberRef = (char*)member->data;
                std::string refStr(memberRef);

                MmsValue* val = MmsValue_getElement(values, index);
                valuesObj.Set(refStr, val ? MmsValueToNapi(env, val) : env.Null());
                index++;
            }
            member = LinkedList_getNext(member);
        }

        result.Set("isValid", true);
        result.Set("values", valuesObj);
        result.Set("count", Napi::Number::New(env, index));
        result.Set("isDeletable", Napi::Boolean::New(env, isDeletable));

        jsResults.Set(i, result);

        // Освобождаем ресурсы
        ClientDataSet_destroy(clientDataSet);
        LinkedList_destroy(members);
    }

    // === Уведомляем через TSFN (минимально, без крашей) ===
    tsfn_.NonBlockingCall([this, count = datasetRefs.size()](Napi::Env env, Napi::Function jsCallback) {
        try {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            eventObj.Set("type", Napi::String::New(env, "data"));
            eventObj.Set("event", count == 1 ? "dataSetRead" : "multipleDataSetsRead");
            eventObj.Set("count", Napi::Number::New(env, (int)count));

            jsCallback.Call({ Napi::String::New(env, "data"), eventObj });
        } catch (...) {
            // Игнорируем ошибки в колбэке — это не критично
        }
    });

    return jsResults;
}

Napi::Value MmsClient::BrowseDataModel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::lock_guard<std::mutex> lock(connMutex_);
    IedClientError error;

    LinkedList deviceList = IedConnection_getLogicalDeviceList(connection_, &error);
    if (error != IED_ERROR_OK || !deviceList) {
        Napi::Error::New(env, "Failed to get logical device list").ThrowAsJavaScriptException();
        return env.Null();
    }

    Napi::Array resultArray = Napi::Array::New(env);
    uint32_t deviceIndex = 0;

    LinkedList device = LinkedList_getNext(deviceList);
    while (device) {
        char* ldName = (char*)device->data;
        if (!ldName) { device = LinkedList_getNext(device); continue; }

        Napi::Object ldObj = Napi::Object::New(env);
        ldObj.Set("name", Napi::String::New(env, ldName));
        Napi::Array lnArray = Napi::Array::New(env);
        uint32_t lnIndex = 0;

        LinkedList logicalNodes = IedConnection_getLogicalDeviceDirectory(connection_, &error, ldName);
        if (error == IED_ERROR_OK && logicalNodes) {
            LinkedList ln = LinkedList_getNext(logicalNodes);
            while (ln) {
                char* lnName = (char*)ln->data;
                if (!lnName) { ln = LinkedList_getNext(ln); continue; }

                std::string lnRef = std::string(ldName) + "/" + lnName;
                Napi::Object lnObj = Napi::Object::New(env);
                lnObj.Set("name", Napi::String::New(env, lnName));

                Napi::Array dsArray = Napi::Array::New(env);
                uint32_t dsIndex = 0;

                LinkedList dataSets = IedConnection_getLogicalNodeDirectory(connection_, &error, lnRef.c_str(), ACSI_CLASS_DATA_SET);
                if (error == IED_ERROR_OK && dataSets) {
                    LinkedList ds = LinkedList_getNext(dataSets);
                    while (ds) {
                        char* dsName = (char*)ds->data;
                        if (!dsName) { ds = LinkedList_getNext(ds); continue; }

                        std::string dsRef = lnRef + "." + dsName;
                        bool isDeletable = false;

                        LinkedList members = IedConnection_getDataSetDirectory(connection_, &error, dsRef.c_str(), &isDeletable);
                        Napi::Object dsObj = Napi::Object::New(env);
                        dsObj.Set("name", Napi::String::New(env, dsName));
                        dsObj.Set("reference", Napi::String::New(env, dsRef));
                        dsObj.Set("isDeletable", Napi::Boolean::New(env, isDeletable));

                        Napi::Array memberArray = Napi::Array::New(env);
                        if (error == IED_ERROR_OK && members) {
                            LinkedList member = LinkedList_getNext(members);
                            uint32_t mIndex = 0;
                            while (member) {
                                char* mRef = (char*)member->data;
                                if (mRef) memberArray.Set(mIndex++, Napi::String::New(env, mRef));
                                member = LinkedList_getNext(member);
                            }
                            LinkedList_destroy(members);
                        }
                        dsObj.Set("members", memberArray);
                        dsArray.Set(dsIndex++, dsObj);
                        ds = LinkedList_getNext(ds);
                    }
                    LinkedList_destroy(dataSets);
                }

                lnObj.Set("dataSets", dsArray);
                lnArray.Set(lnIndex++, lnObj);
                ln = LinkedList_getNext(ln);
            }
            LinkedList_destroy(logicalNodes);
        }

        ldObj.Set("logicalNodes", lnArray);
        resultArray.Set(deviceIndex++, ldObj);
        device = LinkedList_getNext(device);
    }
    LinkedList_destroy(deviceList);

    return resultArray;
}

Napi::Value MmsClient::CreateDataSet(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
        Napi::TypeError::New(env, "Expected datasetRef (string) and dataSetElements (array)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::string datasetRef = info[0].As<Napi::String>().Utf8Value();
    Napi::Array elements = info[1].As<Napi::Array>();
    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("CreateDataSet: Not connected, clientID: %s\n", clientID_.c_str());
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        LinkedList dataSetItems = LinkedList_create();
        for (uint32_t i = 0; i < elements.Length(); i++) {
            if (elements.Get(i).IsString()) {
                std::string element = elements.Get(i).As<Napi::String>().Utf8Value();
                LinkedList_add(dataSetItems, strdup(element.c_str()));
            }
        }
        IedClientError error;
        IedConnection_createDataSet(connection_, &error, datasetRef.c_str(), dataSetItems);
        LinkedList_destroyDeep(dataSetItems, free);
        if (error != IED_ERROR_OK) {
            printf("Failed to create dataset %s, error: %d, clientID: %s\n", datasetRef.c_str(), error, clientID_.c_str());
            std::string errorMsg = "Failed to create dataset: " + std::to_string(error);
            tsfn_.NonBlockingCall([this, errorMsg](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, errorMsg));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in CreateDataSet: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            return env.Undefined();
        }
        tsfn_.NonBlockingCall([this, datasetRef](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "control"));
                eventObj.Set("event", Napi::String::New(env, "dataSetCreated"));
                eventObj.Set("datasetRef", Napi::String::New(env, datasetRef));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in CreateDataSet: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in CreateDataSet: %s, clientID: %s\n", e.what(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Exception in CreateDataSet: ") + e.what()));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in CreateDataSet: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        return env.Undefined();
    }
}

Napi::Value MmsClient::DeleteDataSet(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected datasetRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::string datasetRef = info[0].As<Napi::String>().Utf8Value();
    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("DeleteDataSet: Not connected, clientID: %s\n", clientID_.c_str());
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        IedClientError error;
        IedConnection_deleteDataSet(connection_, &error, datasetRef.c_str());
        if (error != IED_ERROR_OK) {
            printf("Failed to delete dataset %s, error: %d, clientID: %s\n", datasetRef.c_str(), error, clientID_.c_str());
            std::string errorMsg = "Failed to delete dataset: " + std::to_string(error);
            tsfn_.NonBlockingCall([this, errorMsg](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, errorMsg));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in DeleteDataSet: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            return env.Undefined();
        }
        tsfn_.NonBlockingCall([this, datasetRef](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "control"));
                eventObj.Set("event", Napi::String::New(env, "dataSetDeleted"));
                eventObj.Set("datasetRef", Napi::String::New(env, datasetRef));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in DeleteDataSet: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in DeleteDataSet: %s, clientID: %s\n", e.what(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Exception in DeleteDataSet: ") + e.what()));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in DeleteDataSet: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        return env.Undefined();
    }
}

Napi::Value MmsClient::GetDataSetDirectory(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);

    // Validate input: expect a logical node reference (string)
    if (info.Length() < 1 || !info[0].IsString()) {
        printf("GetDataSetDirectory: Invalid input, expected logicalNodeRef (string), clientID: %s\n", clientID_.c_str());
        deferred.Reject(Napi::TypeError::New(env, "Expected logicalNodeRef (string)").Value());
        return deferred.Promise();
    }

    std::string logicalNodeRef = info[0].As<Napi::String>().Utf8Value();
    printf("GetDataSetDirectory: Attempting to retrieve datasets for %s, clientID: %s\n", 
           logicalNodeRef.c_str(), clientID_.c_str());

    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("GetDataSetDirectory: Not connected, clientID: %s\n", clientID_.c_str());
        deferred.Reject(Napi::Error::New(env, "Not connected").Value());
        return deferred.Promise();
    }

    try {
        IedClientError error;
        // Retrieve dataset directory for the logical node
        LinkedList dataSetList = IedConnection_getDataSetDirectory(connection_, &error, logicalNodeRef.c_str(), nullptr);

        if (error != IED_ERROR_OK || dataSetList == nullptr) {
            printf("GetDataSetDirectory: Failed to get dataset directory for %s, error: %d, clientID: %s\n", 
                   logicalNodeRef.c_str(), error, clientID_.c_str());
            tsfn_.NonBlockingCall([this, logicalNodeRef, error](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "Failed to get dataset directory for " + logicalNodeRef + ", error: " + std::to_string(error)));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in GetDataSetDirectory: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            deferred.Reject(Napi::Error::New(env, "Failed to get dataset directory, error: " + std::to_string(error)).Value());
            return deferred.Promise();
        }
  
        // Collect dataset references
        std::vector<std::string> dataSets;
        LinkedList current = dataSetList;
        while (current != nullptr) {
            if (current->data != nullptr) {
                char* dataSetName = (char*)current->data;
                dataSets.push_back(std::string(dataSetName));
                printf("GetDataSetDirectory: Found dataset: %s/%s, clientID: %s\n", 
                       logicalNodeRef.c_str(), dataSetName, clientID_.c_str());
            }
            current = LinkedList_getNext(current);
        }
        LinkedList_destroy(dataSetList);

        // Send event via TSFN
        tsfn_.NonBlockingCall([this, logicalNodeRef, dataSets](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "data"));
                eventObj.Set("event", Napi::String::New(env, "dataSetDirectory"));
                eventObj.Set("logicalNodeRef", Napi::String::New(env, logicalNodeRef));

                Napi::Array dataSetArray = Napi::Array::New(env, dataSets.size());
                for (size_t i = 0; i < dataSets.size(); ++i) {
                    dataSetArray.Set(uint32_t(i), Napi::String::New(env, dataSets[i]));
                }
                eventObj.Set("dataSets", dataSetArray);

                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in GetDataSetDirectory: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });

        // Resolve promise with dataset array
        Napi::Array resultArray = Napi::Array::New(env, dataSets.size());
        for (size_t i = 0; i < dataSets.size(); ++i) {
            resultArray.Set(uint32_t(i), Napi::String::New(env, dataSets[i]));
        }
        printf("GetDataSetDirectory: Successfully retrieved %zu datasets for %s, clientID: %s\n", 
               dataSets.size(), logicalNodeRef.c_str(), clientID_.c_str());
        deferred.Resolve(resultArray);
        return deferred.Promise();

    } catch (const std::exception& e) {
        printf("GetDataSetDirectory: Exception occurred: %s, clientID: %s\n", e.what(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Exception in GetDataSetDirectory: ") + e.what()));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in GetDataSetDirectory: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        deferred.Reject(Napi::Error::New(env, std::string("Exception in GetDataSetDirectory: ") + e.what()).Value());
        return deferred.Promise();
    }
}


/*Napi::Value MmsClient::ReadData(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Expected dataRef (string) or array of strings").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::vector<std::string> dataRefs;

    if (info[0].IsString()) {
        dataRefs.push_back(info[0].As<Napi::String>().Utf8Value());
    } else if (info[0].IsArray()) {
        Napi::Array arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            if (!arr.Get(i).IsString()) {
                Napi::TypeError::New(env, "All elements must be strings").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            dataRefs.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
        }
    } else {
        Napi::TypeError::New(env, "Expected string or array of strings").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        Napi::Error::New(env, "Client not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    Napi::Array results = Napi::Array::New(env, dataRefs.size());

    for (size_t i = 0; i < dataRefs.size(); ++i) {
        {
        const std::string& ref = dataRefs[i];
        Napi::Object result = Napi::Object::New(env);
        result.Set("dataRef", Napi::String::New(env, ref));

        IedClientError error;
        FunctionalConstraint fc = IEC61850_FC_ST;

        // Автоопределение FC (как было)
        if (ref.find(".ctlModel") != std::string::npos) fc = IEC61850_FC_CF;
        else if (ref.find(".Oper") != std::string::npos) fc = IEC61850_FC_CO;
        else if (ref.find(".SPCSO") != std::string::npos) fc = IEC61850_FC_ST;
        else if (ref.find(".AnIn") != std::string::npos) fc = IEC61850_FC_MX;
        else if (ref.find(".NamPlt") != std::string::npos || ref.find(".PhyNam") != std::string::npos) fc = IEC61850_FC_DC;

        MmsValue* value = nullptr;
        std::vector<FunctionalConstraint> fcs = {
            fc, IEC61850_FC_ALL, IEC61850_FC_ST, IEC61850_FC_MX,
            IEC61850_FC_DC, IEC61850_FC_SP, IEC61850_FC_CO, IEC61850_FC_CF
        };

        for (auto tryFc : fcs) {
            value = IedConnection_readObject(connection_, &error, ref.c_str(), tryFc);
            if (error == IED_ERROR_OK && value) break;
            if (value) MmsValue_delete(value);
        }

        if (error != IED_ERROR_OK || !value) {
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "Read failed: error " + std::to_string(error)));
        } else {
            MmsClient::ResultData resData = ConvertMmsValueToResultData(value, ref.substr(ref.rfind(".") + 1));
            result.Set("isValid", true);
            result.Set("value", ResultDataToNapi(env, resData));
            MmsValue_delete(value);
        }

        results.Set(i, result);
    }

    // Отправляем событие через tsfn (опционально)
    tsfn_.NonBlockingCall([this, dataRefs, results](Napi::Env env, Napi::Function jsCallback) {
        try {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            eventObj.Set("type", Napi::String::New(env, "data"));
            eventObj.Set("event", dataRefs.size() == 1 ? "data" : "batchData");

            Napi::Array refsArr = Napi::Array::New(env, dataRefs.size());
            Napi::Array valsArr = Napi::Array::New(env, dataRefs.size());
            for (size_t i = 0; i < dataRefs.size(); ++i) {
                Napi::Object res = results.Get(i).As<Napi::Object>();
                refsArr.Set(i, Napi::String::New(env, dataRefs[i]));
                valsArr.Set(i, res);
            }
            eventObj.Set(dataRefs.size() == 1 ? "dataRef" : "dataRefs", refsArr);
            eventObj.Set("values", valsArr);

            jsCallback.Call({ Napi::String::New(env, "data"), eventObj });
        } catch (...) {}
    });

    // Возвращаем результат синхронно
    return results;
    }
}*/

Napi::Value MmsClient::ReadData(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Expected dataRef or array of dataRefs").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::vector<std::string> dataRefs;
    if (info[0].IsString()) {
        dataRefs.push_back(info[0].As<Napi::String>().Utf8Value());
    } else if (info[0].IsArray()) {
        Napi::Array arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            if (arr.Get(i).IsString()) {
                dataRefs.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
            }
        }
    } else {
        Napi::TypeError::New(env, "Expected string or array").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    Napi::Array results = Napi::Array::New(env, dataRefs.size());

    for (size_t i = 0; i < dataRefs.size(); ++i) {
        const std::string& ref = dataRefs[i];
        Napi::Object item = Napi::Object::New(env);
        item.Set("dataRef", Napi::String::New(env, ref));

        IedClientError error;
        MmsValue* value = nullptr;

        // Попробуем разные FC
        std::vector<FunctionalConstraint> fcs = {
            IEC61850_FC_ST, IEC61850_FC_MX, IEC61850_FC_SP,
            IEC61850_FC_CF, IEC61850_FC_DC, IEC61850_FC_SG,
            IEC61850_FC_CO, IEC61850_FC_ALL
        };

        for (auto fc : fcs) {
            value = IedConnection_readObject(connection_, &error, ref.c_str(), fc);
            if (error == IED_ERROR_OK && value) break;
            if (value) { MmsValue_delete(value); value = nullptr; }
        }

        if (!value || error != IED_ERROR_OK) {
            item.Set("isValid", false);
            item.Set("errorReason", Napi::String::New(env,
                "Read failed (error " + std::to_string(error) + ")"));
            item.Set("value", env.Null());
        } else {
            MmsClient::ResultData resData = ConvertMmsValueToResultData(value, ref);
            item.Set("isValid", true);
            item.Set("value", ResultDataToNapi(env, resData));
            MmsValue_delete(value);
        }

        results.Set(static_cast<uint32_t>(i), item);  // ← ВСЕГДА заполняем
    }

    return results;  // ← синхронный возврат
}



Napi::Value MmsClient::ControlObject(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBoolean()) {
        Napi::TypeError::New(env, "Expected dataRef (string) and value (boolean)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string dataRef = info[0].As<Napi::String>().Utf8Value();
    bool controlValue = info[1].As<Napi::Boolean>().Value();

    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("ControlObject: Not connected, clientID: %s\n", clientID_.c_str());
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        IedClientError error;

        // Read ctlModel
        std::string ctlModelRef = dataRef + ".ctlModel";
        printf("Reading ctlModel from: %s (FC=CF)\n", ctlModelRef.c_str());

        MmsValue* ctlModelValue = IedConnection_readObject(
            connection_, &error,
            ctlModelRef.c_str(),
            IEC61850_FC_CF
        );

        int32_t ctlModel = 0;
        if (error == IED_ERROR_OK && ctlModelValue != nullptr) {
            ctlModel = MmsValue_toInt32(ctlModelValue);
            printf("ctlModel read successfully: %d\n", ctlModel);
            MmsValue_delete(ctlModelValue);
        } else {
            printf("Failed to read ctlModel (error: %d). Falling back to ctlModel=1\n", error);
            ctlModel = 1; // Fallback to direct-with-normal-security
        }

        // Check if control is allowed
        if (ctlModel == 0) {
            printf("Control blocked: ctlModel=status-only\n");
            tsfn_.NonBlockingCall([this, dataRef](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "Control blocked for " + dataRef + ": status-only"));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            return env.Undefined();
        }

        // Prepare control operation
        std::string operRef = dataRef + ".Oper";
        std::string stValRef = dataRef + ".stVal";
        printf("Attempting control on: %s\n", operRef.c_str());

        // Helper function to read and send status update
        auto sendStatusUpdate = [&](bool success) {
            IedClientError stError;
            MmsValue* stVal = IedConnection_readObject(connection_, &stError, stValRef.c_str(), IEC61850_FC_ST);
            if (stError == IED_ERROR_OK && stVal != nullptr) {
                bool state = MmsValue_getBoolean(stVal);
                printf("New status of %s: %d\n", stValRef.c_str(), state);
                tsfn_.NonBlockingCall([this, stValRef, state, success](Napi::Env env, Napi::Function jsCallback) {
                    try {
                        Napi::Object eventObj = Napi::Object::New(env);
                        eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                        eventObj.Set("type", Napi::String::New(env, success ? "data" : "error"));
                        eventObj.Set("dataRef", Napi::String::New(env, stValRef));
                        eventObj.Set("value", Napi::Boolean::New(env, state));
                        eventObj.Set("isValid", Napi::Boolean::New(env, true));
                        if (!success) {
                            eventObj.Set("reason", Napi::String::New(env, "Control operation failed, current status reported"));
                        }
                        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                    } catch (const Napi::Error& e) {
                        printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                    }
                });
                MmsValue_delete(stVal);
            } else {
                printf("Failed to read status for %s, error: %d\n", stValRef.c_str(), stError);
                tsfn_.NonBlockingCall([this, stValRef](Napi::Env env, Napi::Function jsCallback) {
                    try {
                        Napi::Object eventObj = Napi::Object::New(env);
                        eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                        eventObj.Set("type", Napi::String::New(env, "error"));
                        eventObj.Set("reason", Napi::String::New(env, "Failed to read status for " + stValRef + " after control"));
                        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                    } catch (const Napi::Error& e) {
                        printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                    }
                });
            }
        };

        // Command termination handler for enhanced security
        auto commandTerminationHandler = [](void* parameter, ControlObjectClient control) {
            MmsClient* client = static_cast<MmsClient*>(parameter);
            LastApplError lastApplError = ControlObjectClient_getLastApplError(control);
            std::string status = (lastApplError.error != 0) ? "CommandTermination-" : "CommandTermination+";
            printf("%s\n", status.c_str());
            if (lastApplError.error != 0) {
                printf(" LastApplError: %i\n", lastApplError.error);
                printf("      addCause: %i\n", lastApplError.addCause);
            }
            client->tsfn_.NonBlockingCall([client, status, lastApplError](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, client->clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "control"));
                    eventObj.Set("event", Napi::String::New(env, status));
                    if (lastApplError.error != 0) {
                        eventObj.Set("error", Napi::Number::New(env, lastApplError.error));
                        eventObj.Set("addCause", Napi::Number::New(env, lastApplError.addCause));
                    }
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), client->clientID_.c_str());
                }
            });
        };

        // Create control object
        ControlObjectClient control = ControlObjectClient_create(dataRef.c_str(), connection_);
        if (!control) {
            printf("Control object %s not found in server\n", operRef.c_str());
            tsfn_.NonBlockingCall([this, dataRef](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "Failed to create control object for " + dataRef));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            return env.Undefined();
        }

        MmsValue* ctlVal = MmsValue_newBoolean(controlValue);
        if (!ctlVal) {
            printf("Failed to create MmsValue for control\n");
            ControlObjectClient_destroy(control);
            tsfn_.NonBlockingCall([this, dataRef](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "Failed to create control value for " + dataRef));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            return env.Undefined();
        }

        bool operateSuccess = false;

        // Direct control (ctlModel = 1)
        if (ctlModel == 1) {
            printf("Using DIRECT control (ctlModel=%d)\n", ctlModel);
            ControlObjectClient_setOrigin(control, NULL, 3);
            operateSuccess = ControlObjectClient_operate(control, ctlVal, 1);
        }
        // SBO control (ctlModel = 2)
        else if (ctlModel == 2) {
            printf("Using SBO control (ctlModel=%d)\n", ctlModel);
            if (ControlObjectClient_select(control)) {
                operateSuccess = ControlObjectClient_operate(control, ctlVal, 0);
            } else {
                printf("SBO select failed for %s\n", operRef.c_str());
                MmsValue_delete(ctlVal);
                ControlObjectClient_destroy(control);
                tsfn_.NonBlockingCall([this, dataRef](Napi::Env env, Napi::Function jsCallback) {
                    try {
                        Napi::Object eventObj = Napi::Object::New(env);
                        eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                        eventObj.Set("type", Napi::String::New(env, "error"));
                        eventObj.Set("reason", Napi::String::New(env, "SBO select failed for " + dataRef));
                        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                    } catch (const Napi::Error& e) {
                        printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                    }
                });
                return env.Undefined();
            }
        }
        // Direct control with enhanced security (ctlModel = 3)
        else if (ctlModel == 3) {
            printf("Using DIRECT control with enhanced security (ctlModel=%d)\n", ctlModel);
            ControlObjectClient_setCommandTerminationHandler(control, commandTerminationHandler, this);
            ControlObjectClient_setOrigin(control, nullptr, 3);
            operateSuccess = ControlObjectClient_operate(control, ctlVal, 0);
            // Wait for command termination
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        // SBO with enhanced security (ctlModel = 4)
        else if (ctlModel == 4) {
            printf("Using SBO control with enhanced security (ctlModel=%d)\n", ctlModel);
            ControlObjectClient_setCommandTerminationHandler(control, commandTerminationHandler, this);
            if (ControlObjectClient_selectWithValue(control, ctlVal)) {
                operateSuccess = ControlObjectClient_operate(control, ctlVal, 0);
                // Wait for command termination
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            } else {
                printf("SBO selectWithValue failed for %s\n", operRef.c_str());
                MmsValue_delete(ctlVal);
                ControlObjectClient_destroy(control);
                tsfn_.NonBlockingCall([this, dataRef](Napi::Env env, Napi::Function jsCallback) {
                    try {
                        Napi::Object eventObj = Napi::Object::New(env);
                        eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                        eventObj.Set("type", Napi::String::New(env, "error"));
                        eventObj.Set("reason", Napi::String::New(env, "SBO selectWithValue failed for " + dataRef));
                        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                    } catch (const Napi::Error& e) {
                        printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                    }
                });
                return env.Undefined();
            }
        }

        // Send control operation result
        if (operateSuccess) {
            printf("Control operation succeeded for %s\n", operRef.c_str());
            tsfn_.NonBlockingCall([this, dataRef, controlValue](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "control"));
                    eventObj.Set("dataRef", Napi::String::New(env, dataRef));
                    eventObj.Set("value", Napi::Boolean::New(env, controlValue));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
        } else {
            printf("Control operation failed for %s\n", operRef.c_str());
            tsfn_.NonBlockingCall([this, dataRef](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "Control failed for " + dataRef));
                    jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
        }

        // Cleanup
        MmsValue_delete(ctlVal);
        ControlObjectClient_destroy(control);

        // Send status update
        sendStatusUpdate(operateSuccess);

        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in ControlObject: %s\n", e.what());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Exception: ") + e.what()));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in ControlObject: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        return env.Undefined();
    }
}

Napi::Value MmsClient::GetLogicalDevices(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("GetLogicalDevices: Not connected, clientID: %s\n", clientID_.c_str());
        deferred.Reject(Napi::Error::New(env, "Not connected").Value());
        return deferred.Promise();
    }
    try {
        IedClientError error;
        struct DataAttribute {
            std::string name;
            MmsType type;
            std::string value;
            bool isValid;
        };
        struct DataObject {
            std::string name;
            std::vector<DataAttribute> attributes;
            std::vector<DataObject> subObjects;
        };
        struct LogicalNode {
            std::string name;
            std::vector<DataObject> dataObjects;
        };
        struct LogicalDevice {
            std::string name;
            std::vector<LogicalNode> logicalNodes;
        };
        std::vector<LogicalDevice> logicalDevices;
        auto readAttributeValue = [&](const std::string& ref, FunctionalConstraint fc) -> DataAttribute {
            DataAttribute attr;
            attr.name = ref.substr(ref.rfind(".") + 1);
            attr.isValid = false;
            attr.value = "unreadable";
            IedClientError readError;
            MmsValue* value = nullptr;
            std::vector<FunctionalConstraint> fcs = {
                fc, IEC61850_FC_ALL, IEC61850_FC_ST, IEC61850_FC_MX,
                IEC61850_FC_DC, IEC61850_FC_SP, IEC61850_FC_CO, IEC61850_FC_CF
            };
            for (auto tryFc : fcs) {
                value = IedConnection_readObject(connection_, &readError, ref.c_str(), tryFc);
                if (readError == IED_ERROR_OK && value != nullptr) {
                    printf("readAttributeValue: Succeeded with FC %d for %s, clientID: %s\n", tryFc, ref.c_str(), clientID_.c_str());
                    break;
                }
                printf("readAttributeValue: Failed with FC %d for %s, error: %d, clientID: %s\n", tryFc, ref.c_str(), readError, clientID_.c_str());
            }
            if (readError == IED_ERROR_OK && value != nullptr) {
                ResultData resData = ConvertMmsValueToResultData(value, attr.name);
                attr.type = resData.type;
                attr.value = ResultDataToString(resData);
                attr.isValid = resData.isValid;
                printf("readAttributeValue: Value for %s: %s, type: %d, isValid: %d, clientID: %s\n",
                       ref.c_str(), attr.value.c_str(), attr.type, attr.isValid, clientID_.c_str());
                MmsValue_delete(value);
            } else {
                std::string errorMsg;
                switch (readError) {
                    case IED_ERROR_OBJECT_DOES_NOT_EXIST: errorMsg = "Object does not exist"; break;
                    case IED_ERROR_ACCESS_DENIED: errorMsg = "Access denied"; break;
                    case IED_ERROR_TYPE_INCONSISTENT: errorMsg = "Type inconsistent"; break;
                    case IED_ERROR_OBJECT_ACCESS_UNSUPPORTED: errorMsg = "Object access unsupported"; break;
                    default: errorMsg = "Unknown error: " + std::to_string(readError);
                }
                attr.value = errorMsg;
                printf("readAttributeValue: Failed to read %s with all FCs, final error: %s, clientID: %s\n", ref.c_str(), errorMsg.c_str(), clientID_.c_str());
            }
            return attr;
        };
        std::function<void(const std::string&, DataObject&, FunctionalConstraint)> processDataObject;
        processDataObject = [&](const std::string& parentRef, DataObject& dataObj, FunctionalConstraint fc) {
            IedClientError doError;
            LinkedList attrList = IedConnection_getDataDirectory(connection_, &doError, parentRef.c_str());
            if (doError == IED_ERROR_OK && attrList != nullptr) {
                printf("Successfully retrieved data directory for %s, clientID: %s\n", parentRef.c_str(), clientID_.c_str());
                LinkedList currentAttr = attrList;
                while (currentAttr != nullptr) {
                    if (currentAttr->data != nullptr) {
                        char* attrName = (char*)currentAttr->data;
                        std::string attrRef = parentRef + "." + attrName;
                        FunctionalConstraint attrFc = fc;
                        if (std::string(attrName) == "Oper") {
                            attrFc = IEC61850_FC_CO;
                        } else if (std::string(attrName) == "ctlModel") {
                            attrFc = IEC61850_FC_CF;
                        } else if (std::string(attrName).find("NamPlt") != std::string::npos || std::string(attrName).find("PhyNam") != std::string::npos) {
                            attrFc = IEC61850_FC_DC;
                        } else if (std::string(attrName).find("Mod") != std::string::npos || std::string(attrName).find("Proxy") != std::string::npos) {
                            attrFc = IEC61850_FC_ST;
                        } else if (std::string(attrName).find("SPCSO") != std::string::npos) {
                            attrFc = IEC61850_FC_ST;
                        } else if (std::string(attrName).find("AnIn") != std::string::npos) {
                            attrFc = IEC61850_FC_MX;
                        }
                        DataAttribute attr = readAttributeValue(attrRef, attrFc);
                        dataObj.attributes.push_back(attr);
                        IedClientError subError;
                        LinkedList subAttrList = IedConnection_getDataDirectory(connection_, &subError, attrRef.c_str());
                        if (subError == IED_ERROR_OK && subAttrList != nullptr) {
                            DataObject subObj;
                            subObj.name = attrName;
                            processDataObject(attrRef, subObj, attrFc);
                            dataObj.subObjects.push_back(subObj); // Всегда добавляем подобъект
                            LinkedList_destroy(subAttrList);
                        }
                    }
                    currentAttr = LinkedList_getNext(currentAttr);
                }
                LinkedList_destroy(attrList);
            } else {
                printf("Failed to get data directory for %s, error: %d, clientID: %s\n", parentRef.c_str(), doError, clientID_.c_str());
            }
        };
        // Получение списка логических устройств
        LinkedList deviceList = IedConnection_getLogicalDeviceList(connection_, &error);
        if (error != IED_ERROR_OK || deviceList == nullptr) {
            printf("Failed to get logical device list, error: %d, clientID: %s\n", error, clientID_.c_str());
            tsfn_.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "Failed to get logical device list"));
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in GetLogicalDevices: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            deferred.Reject(Napi::Error::New(env, "Failed to get logical device list").Value());
            return deferred.Promise();
        }
        LinkedList currentDevice = deviceList;
        while (currentDevice != nullptr) {
            if (currentDevice->data != nullptr) {
                char* deviceName = (char*)currentDevice->data;
                LogicalDevice ld;
                ld.name = std::string(deviceName);
                printf("Processing logical device %s, clientID: %s\n", ld.name.c_str(), clientID_.c_str());
                // Получение списка логических узлов
                LinkedList nodeList = IedConnection_getLogicalDeviceDirectory(connection_, &error, ld.name.c_str());
                if (error != IED_ERROR_OK || nodeList == nullptr) {
                    printf("Failed to get logical node list for %s, error: %d, clientID: %s\n", ld.name.c_str(), error, clientID_.c_str());
                    currentDevice = LinkedList_getNext(currentDevice);
                    continue;
                }
                LinkedList currentNode = nodeList;
                while (currentNode != nullptr) {
                    if (currentNode->data != nullptr) {
                        char* nodeName = (char*)currentNode->data;
                        LogicalNode ln;
                        ln.name = std::string(nodeName);
                        std::string nodeRef = ld.name + "/" + ln.name;
                        printf("Processing logical node %s, clientID: %s\n", nodeRef.c_str(), clientID_.c_str());
                        // Получение списка списка объектов данных
                        LinkedList doList = IedConnection_getLogicalNodeVariables(connection_, &error, nodeRef.c_str());
                        if (error != IED_ERROR_OK || doList == nullptr) {
                            printf("Failed to get data object list for %s, error: %d, clientID: %s\n", nodeRef.c_str(), error, clientID_.c_str());
                            currentNode = LinkedList_getNext(currentNode);
                            continue;
                        }
                        LinkedList currentDo = doList;
                        while (currentDo != nullptr) {
                            if (currentDo->data != nullptr) {
                                char* doName = (char*)currentDo->data;
                                std::string doRef = nodeRef + "." + doName;
                                DataObject dataObj;
                                dataObj.name = doName;
                                // Определение подходящего FC на основе имени объекта
                                FunctionalConstraint doFc = IEC61850_FC_ALL;
                                if (std::string(doName).find("Oper") != std::string::npos) {
                                    doFc = IEC61850_FC_CO;
                                } else if (std::string(doName).find("ctlModel") != std::string::npos) {
                                    doFc = IEC61850_FC_CF;
                                } else if (std::string(doName).find("NamPlt") != std::string::npos || std::string(doName).find("PhyNam") != std::string::npos) {
                                    doFc = IEC61850_FC_DC;
                                } else if (std::string(doName).find("Mod") != std::string::npos || std::string(doName).find("Proxy") != std::string::npos) {
                                    doFc = IEC61850_FC_ST;
                                } else if (std::string(doName).find("SPCSO") != std::string::npos) {
                                    doFc = IEC61850_FC_ST;
                                } else if (std::string(doName).find("AnIn") != std::string::npos) {
                                    doFc = IEC61850_FC_MX;
                                } else if (std::string(doName).find("EventsBRCB") != std::string::npos || std::string(doName).find("Measurements") != std::string::npos) {
                                    doFc = IEC61850_FC_BR;
                                } else if (std::string(doName).find("EventsRCB") != std::string::npos || std::string(doName).find("EventsIndexed") != std::string::npos) {
                                    doFc = IEC61850_FC_RP;
                                }
                                processDataObject(doRef, dataObj, doFc);
                                ln.dataObjects.push_back(dataObj); // Всегда добавляем объект данных
                            }
                            currentDo = LinkedList_getNext(currentDo);
                        }
                        LinkedList_destroy(doList);
                        ld.logicalNodes.push_back(ln); // Всегда добавляем логический узел
                    }
                    currentNode = LinkedList_getNext(currentNode);
                }
                LinkedList_destroy(nodeList);
                logicalDevices.push_back(ld); // Всегда добавляем логическое устройство
            }
            currentDevice = LinkedList_getNext(currentDevice);
        }
        LinkedList_destroy(deviceList);
        if (logicalDevices.empty()) {
            printf("No valid logical devices found, clientID: %s\n", clientID_.c_str());
            tsfn_.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
                try {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, "No valid logical devices found"));
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                } catch (const Napi::Error& e) {
                    printf("N-API Callback Error in GetLogicalDevices: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                }
            });
            deferred.Reject(Napi::Error::New(env, "No valid logical devices found").Value());
            return deferred.Promise();
        }
        tsfn_.NonBlockingCall([this, logicalDevices](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "data"));
                eventObj.Set("event", Napi::String::New(env, "logicalDevices"));
                auto toNapiObject = [](Napi::Env env, const auto& obj, auto toNapiFunc) -> Napi::Value {
                    Napi::Object napiObj = Napi::Object::New(env);
                    napiObj.Set("name", Napi::String::New(env, obj.name));
                    if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, DataAttribute>) {
                        napiObj.Set("type", Napi::Number::New(env, obj.type));
                        napiObj.Set("value", Napi::String::New(env, obj.value));
                        napiObj.Set("isValid", Napi::Boolean::New(env, obj.isValid));
                    } else if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, DataObject>) {
                        Napi::Array attrs = Napi::Array::New(env, obj.attributes.size());
                        for (size_t i = 0; i < obj.attributes.size(); i++) {
                            attrs.Set(uint32_t(i), toNapiFunc(env, obj.attributes[i], toNapiFunc));
                        }
                        napiObj.Set("attributes", attrs);
                        Napi::Array subObjs = Napi::Array::New(env, obj.subObjects.size());
                        for (size_t i = 0; i < obj.subObjects.size(); i++) {
                            subObjs.Set(uint32_t(i), toNapiFunc(env, obj.subObjects[i], toNapiFunc));
                        }
                        napiObj.Set("subObjects", subObjs);
                    } else if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, LogicalNode>) {
                        Napi::Array dataObjs = Napi::Array::New(env, obj.dataObjects.size());
                        for (size_t i = 0; i < obj.dataObjects.size(); i++) {
                            dataObjs.Set(uint32_t(i), toNapiFunc(env, obj.dataObjects[i], toNapiFunc));
                        }
                        napiObj.Set("dataObjects", dataObjs);
                    } else {
                        Napi::Array nodes = Napi::Array::New(env, obj.logicalNodes.size());
                        for (size_t i = 0; i < obj.logicalNodes.size(); i++) {
                            nodes.Set(uint32_t(i), toNapiFunc(env, obj.logicalNodes[i], toNapiFunc));
                        }
                        napiObj.Set("logicalNodes", nodes);
                    }
                    return napiObj;
                };
                Napi::Array devicesArray = Napi::Array::New(env, logicalDevices.size());
                for (size_t i = 0; i < logicalDevices.size(); i++) {
                    devicesArray.Set(uint32_t(i), toNapiObject(env, logicalDevices[i], toNapiObject));
                }
                eventObj.Set("logicalDevices", devicesArray);
                std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                jsCallback.Call(args);
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in GetLogicalDevices: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        Napi::Array resultArray = Napi::Array::New(env, logicalDevices.size());
        for (size_t i = 0; i < logicalDevices.size(); i++) {
            auto toNapiObject = [](Napi::Env env, const auto& obj, auto toNapiFunc) -> Napi::Value {
                Napi::Object napiObj = Napi::Object::New(env);
                napiObj.Set("name", Napi::String::New(env, obj.name));
                if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, DataAttribute>) {
                    napiObj.Set("type", Napi::Number::New(env, obj.type));
                    napiObj.Set("value", Napi::String::New(env, obj.value));
                    napiObj.Set("isValid", Napi::Boolean::New(env, obj.isValid));
                } else if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, DataObject>) {
                    Napi::Array attrs = Napi::Array::New(env, obj.attributes.size());
                    for (size_t i = 0; i < obj.attributes.size(); i++) {
                        attrs.Set(uint32_t(i), toNapiFunc(env, obj.attributes[i], toNapiFunc));
                    }
                    napiObj.Set("attributes", attrs);
                    Napi::Array subObjs = Napi::Array::New(env, obj.subObjects.size());
                    for (size_t i = 0; i < obj.subObjects.size(); i++) {
                        subObjs.Set(uint32_t(i), toNapiFunc(env, obj.subObjects[i], toNapiFunc));
                    }
                    napiObj.Set("subObjects", subObjs);
                } else if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, LogicalNode>) {
                    Napi::Array dataObjs = Napi::Array::New(env, obj.dataObjects.size());
                    for (size_t i = 0; i < obj.dataObjects.size(); i++) {
                        dataObjs.Set(uint32_t(i), toNapiFunc(env, obj.dataObjects[i], toNapiFunc));
                    }
                    napiObj.Set("dataObjects", dataObjs);
                } else {
                    Napi::Array nodes = Napi::Array::New(env, obj.logicalNodes.size());
                    for (size_t i = 0; i < obj.logicalNodes.size(); i++) {
                        nodes.Set(uint32_t(i), toNapiFunc(env, obj.logicalNodes[i], toNapiFunc));
                    }
                    napiObj.Set("logicalNodes", nodes);
                }
                return napiObj;
            };
            resultArray.Set(uint32_t(i), toNapiObject(env, logicalDevices[i], toNapiObject));
        }
        deferred.Resolve(resultArray);
        return deferred.Promise();
    } catch (const std::exception& e) {
        printf("Exception in GetLogicalDevices: %s, clientID: %s\n", e.what(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Exception in GetLogicalDevices: ") + e.what()));
                std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                jsCallback.Call(args);
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in GetLogicalDevices: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        deferred.Reject(Napi::Error::New(env, std::string("Exception in GetLogicalDevices: ") + e.what()).Value());
        return deferred.Promise();
    }
}

Napi::Value MmsClient::Close(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);

    try {
        {
            std::lock_guard<std::mutex> lock(connMutex_);
            if (running_) {
                running_ = false;
                if (connected_) {
                    printf("Close called by client, clientID: %s\n", clientID_.c_str());
                    IedConnection_close(connection_);
                    connected_ = false;
                }
            }
        }

        if (thread_.joinable()) {
            thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(connMutex_);
            if (connection_) {
                printf("Destroying connection, clientID: %s\n", clientID_.c_str());
                IedConnection_destroy(connection_);
                connection_ = nullptr;
            }
            if (tsfn_) {
                printf("Releasing TSFN, clientID: %s\n", clientID_.c_str());
                tsfn_.Release();
                tsfn_ = Napi::ThreadSafeFunction();
            }
        }

        deferred.Resolve(Napi::Boolean::New(env, true));
    } catch (const std::exception& e) {
        printf("Exception in Close: %s, clientID: %s\n", e.what(), clientID_.c_str());
        deferred.Reject(Napi::Error::New(env, std::string("Close failed: ") + e.what()).Value());
    }

    return deferred.Promise();
}

Napi::Value MmsClient::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex_);
    Napi::Object status = Napi::Object::New(env);
    status.Set("connected", Napi::Boolean::New(env, connected_));
    status.Set("clientID", Napi::String::New(env, clientID_.c_str()));
    return status;
}

void MmsClient::ReportCallback(void* parameter, ClientReport report) {
    MmsClient* client = static_cast<MmsClient*>(parameter);
    std::string rcbRef = ClientReport_getRcbReference(report) ? ClientReport_getRcbReference(report) : "unknown";
    std::string rptId = ClientReport_getRptId(report) ? ClientReport_getRptId(report) : "unknown";
    printf("Received report for %s with rptId %s, clientID: %s\n", rcbRef.c_str(), rptId.c_str(), client->clientID_.c_str());
    auto it = client->activeReports_.find(rcbRef);
    if (it == client->activeReports_.end()) {
        printf("No active report info found for %s, clientID: %s\n", rcbRef.c_str(), client->clientID_.c_str());
        return;
    }
    LinkedList dataSetDirectory = it->second.dataSetDirectory;
    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);
    std::vector<ResultData> reportValues;
    std::vector<int> reasonsForInclusion;
    if (dataSetDirectory && dataSetValues) {
        int dataSetSize = LinkedList_size(dataSetDirectory);
        for (int i = 0; i < dataSetSize; i++) {
            ReasonForInclusion reason = ClientReport_getReasonForInclusion(report, i);
            reasonsForInclusion.push_back(reason);
            if (reason != IEC61850_REASON_NOT_INCLUDED) {
                MmsValue* value = MmsValue_getElement(dataSetValues, i);
                if (value) {
                    // Используем общую функцию конвертации
                    ResultData resultData = ConvertMmsValueToResultData(value, "reportValue[" + std::to_string(i) + "]");
                    reportValues.push_back(resultData);
                }
            }
        }
    }
    uint64_t timestamp = 0;
    if (ClientReport_hasTimestamp(report)) {
        timestamp = ClientReport_getTimestamp(report);
    }
    client->tsfn_.NonBlockingCall([client, rcbRef, rptId, reportValues, reasonsForInclusion, timestamp](Napi::Env env, Napi::Function jsCallback) {
        try {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, client->clientID_.c_str()));
            eventObj.Set("type", Napi::String::New(env, "data"));
            eventObj.Set("event", Napi::String::New(env, "report"));
            eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
            eventObj.Set("rptId", Napi::String::New(env, rptId));
            if (timestamp > 0) {
                time_t time = timestamp / 1000;
                char timeStr[64];
                strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", gmtime(&time));
                std::string timeString = std::string(timeStr) + "." + std::to_string(timestamp % 1000);
                eventObj.Set("timestamp", Napi::String::New(env, timeString));
            }
            Napi::Array valuesArray = Napi::Array::New(env, reportValues.size());
            for (size_t i = 0; i < reportValues.size(); i++) {
                valuesArray.Set(uint32_t(i), ResultDataToNapi(env, reportValues[i]));
            }
            eventObj.Set("values", valuesArray);
            Napi::Array reasonsArray = Napi::Array::New(env, reasonsForInclusion.size());
            for (size_t i = 0; i < reasonsForInclusion.size(); i++) {
                reasonsArray.Set(uint32_t(i), Napi::Number::New(env, reasonsForInclusion[i]));
            }
            eventObj.Set("reasonsForInclusion", reasonsArray);
            jsCallback.Call({Napi::String::New(env, "data"), eventObj});
        } catch (const Napi::Error& e) {
            printf("N-API Callback Error in ReportCallback: %s, clientID: %s\n", e.Message().c_str(), client->clientID_.c_str());
        }
    });
}

Napi::Value MmsClient::EnableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string) and datasetRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();
    std::string datasetRef = info[1].As<Napi::String>().Utf8Value();

    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("EnableReporting: Not connected, clientID: %s\n", clientID_.c_str());
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // Проверяем, не включена ли уже отчётность
    if (activeReports_.find(rcbRef) != activeReports_.end()) {
        printf("EnableReporting: Report already enabled for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
        Napi::Error::New(env, "Report already enabled for " + rcbRef).ThrowAsJavaScriptException();
        return env.Undefined();
    }

    IedClientError error;

    // 1. Читаем DataSet Directory (для сопоставления индексов в отчётах)
    LinkedList dataSetDirectory = IedConnection_getDataSetDirectory(connection_, &error, datasetRef.c_str(), nullptr);
    if (error != IED_ERROR_OK || !dataSetDirectory) {
        printf("EnableReporting: Failed to get dataset directory for %s, error: %d\n", datasetRef.c_str(), error);
        tsfn_.NonBlockingCall([this, datasetRef, error](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            obj.Set("type", Napi::String::New(env, "error"));
            obj.Set("reason", Napi::String::New(env, "Failed to get dataset directory for " + datasetRef + ", error: " + std::to_string(error)));
            jsCallback.Call({ Napi::String::New(env, "data"), obj });
        });
        return env.Undefined();
    }

    // 2. Читаем сам DataSet (нужен для освобождения памяти)
    ClientDataSet clientDataSet = IedConnection_readDataSetValues(connection_, &error, datasetRef.c_str(), nullptr);
    if (error != IED_ERROR_OK || !clientDataSet) {
        printf("EnableReporting: Failed to read dataset %s, error: %d\n", datasetRef.c_str(), error);
        LinkedList_destroy(dataSetDirectory);
        tsfn_.NonBlockingCall([this, datasetRef, error](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            obj.Set("type", Napi::String::New(env, "error"));
            obj.Set("reason", Napi::String::New(env, "Failed to read dataset " + datasetRef + ", error: " + std::to_string(error)));
            jsCallback.Call({ Napi::String::New(env, "data"), obj });
        });
        return env.Undefined();
    }

    // 3. Читаем RCB
    ClientReportControlBlock rcb = IedConnection_getRCBValues(connection_, &error, rcbRef.c_str(), nullptr);
    if (error != IED_ERROR_OK || !rcb) {
        printf("EnableReporting: Failed to get RCB %s, error: %d\n", rcbRef.c_str(), error);
        ClientDataSet_destroy(clientDataSet);
        LinkedList_destroy(dataSetDirectory);
        tsfn_.NonBlockingCall([this, rcbRef, error](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            obj.Set("type", Napi::String::New(env, "error"));
            obj.Set("reason", Napi::String::New(env, "Failed to get RCB " + rcbRef + ", error: " + std::to_string(error)));
            jsCallback.Call({ Napi::String::New(env, "data"), obj });
        });
        return env.Undefined();
    }

    // 4. Настраиваем RCB
    ClientReportControlBlock_setResv(rcb, true);
    ClientReportControlBlock_setTrgOps(rcb, TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_INTEGRITY | TRG_OPT_GI);
    ClientReportControlBlock_setDataSetReference(rcb, datasetRef.c_str());
    ClientReportControlBlock_setRptEna(rcb, true);
    ClientReportControlBlock_setIntgPd(rcb, 3000);  // каждые 3 сек
    ClientReportControlBlock_setGI(rcb, true);      // включить GI

    // 5. СОХРАНЯЕМ ReportInfo ДО установки хендлера!
    ReportInfo reportInfo;
    reportInfo.rcb = rcb;
    reportInfo.dataSet = clientDataSet;
    reportInfo.dataSetDirectory = dataSetDirectory;
    reportInfo.rcbRef = rcbRef;
    activeReports_[rcbRef] = reportInfo;

    // 6. УСТАНАВЛИВАЕМ ХЕНДЛЕР ПОСЛЕ СОХРАНЕНИЯ ReportInfo!
    IedConnection_installReportHandler(
        connection_,
        rcbRef.c_str(),
        ClientReportControlBlock_getRptId(rcb),
        ReportCallback,
        this
    );

    // 7. Пишем параметры RCB (включая RptEna и GI)
    IedConnection_setRCBValues(connection_, &error, rcb, RCB_ELEMENT_TRG_OPS | RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_GI | RCB_ELEMENT_INTG_PD, true);

    if (error != IED_ERROR_OK) {
        printf("EnableReporting: Failed to set RCB values, error: %d\n", error);
        activeReports_.erase(rcbRef);
        ClientReportControlBlock_destroy(rcb);
        ClientDataSet_destroy(clientDataSet);
        LinkedList_destroy(dataSetDirectory);
        tsfn_.NonBlockingCall([this, rcbRef, error](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            obj.Set("type", Napi::String::New(env, "error"));
            obj.Set("reason", Napi::String::New(env, "Failed to set RCB values for " + rcbRef + ", error: " + std::to_string(error)));
            jsCallback.Call({ Napi::String::New(env, "data"), obj });
        });
        return env.Undefined();
    }

    printf("EnableReporting: Successfully enabled reporting for %s → %s\n", rcbRef.c_str(), datasetRef.c_str());

    // 8. Отправляем событие
    tsfn_.NonBlockingCall([this, rcbRef](Napi::Env env, Napi::Function jsCallback) {
        try {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
            obj.Set("type", Napi::String::New(env, "control"));
            obj.Set("event", Napi::String::New(env, "reportingEnabled"));
            obj.Set("rcbRef", Napi::String::New(env, rcbRef));
            jsCallback.Call({ Napi::String::New(env, "data"), obj });
        } catch (...) {}
    });

    return env.Undefined();
}

Napi::Value MmsClient::DisableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();

    std::lock_guard<std::mutex> lock(connMutex_);
    if (!connected_) {
        printf("DisableReporting: Not connected, clientID: %s\n", clientID_.c_str());
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        auto it = activeReports_.find(rcbRef);
        if (it == activeReports_.end()) {
            printf("DisableReporting: No active report found for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
            Napi::Error::New(env, "No active report for " + rcbRef).ThrowAsJavaScriptException();
            return env.Undefined();
        }

        ReportInfo& reportInfo = it->second;
        if (reportInfo.rcb) {
            ClientReportControlBlock_setRptEna(reportInfo.rcb, false);
            IedClientError error;
            IedConnection_setRCBValues(connection_, &error, reportInfo.rcb, RCB_ELEMENT_RPT_ENA, true);
            if (error != IED_ERROR_OK) {
                printf("DisableReporting: Failed to disable reporting for %s, error: %d, clientID: %s\n", rcbRef.c_str(), error, clientID_.c_str());
                tsfn_.NonBlockingCall([this, rcbRef, error](Napi::Env env, Napi::Function jsCallback) {
                    try {
                        Napi::Object eventObj = Napi::Object::New(env);
                        eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                        eventObj.Set("type", Napi::String::New(env, "error"));
                        eventObj.Set("reason", Napi::String::New(env, "Failed to disable reporting for " + rcbRef + ", error: " + std::to_string(error)));
                        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                    } catch (const Napi::Error& e) {
                        printf("N-API Callback Error in DisableReporting: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                    }
                });
            }
            ClientReportControlBlock_destroy(reportInfo.rcb);
            reportInfo.rcb = nullptr;
        }
        if (reportInfo.dataSet) {
            ClientDataSet_destroy(reportInfo.dataSet);
            reportInfo.dataSet = nullptr;
        }
        if (reportInfo.dataSetDirectory) {
            LinkedList_destroy(reportInfo.dataSetDirectory);
            reportInfo.dataSetDirectory = nullptr;
        }

        activeReports_.erase(it);
        printf("DisableReporting: Successfully disabled reporting for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, rcbRef](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "control"));
                eventObj.Set("event", Napi::String::New(env, "reportingDisabled"));
                eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in DisableReporting: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });

        return env.Undefined();
    } catch (const std::exception& e) {
        printf("DisableReporting: Exception occurred: %s, clientID: %s\n", e.what(), clientID_.c_str());
        tsfn_.NonBlockingCall([this, e](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("Exception in DisableReporting: ") + e.what()));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const Napi::Error& e) {
                printf("N-API Callback Error in DisableReporting: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
            }
        });
        return env.Undefined();
    }
}