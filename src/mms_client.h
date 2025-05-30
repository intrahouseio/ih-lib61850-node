/*#ifndef MMS_CLIENT_H
#define MMS_CLIENT_H

#include <napi.h>
//#include "hal_thread.h"
//#include "hal_time.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <iec61850_client.h>

class MmsClient : public Napi::ObjectWrap<MmsClient> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    MmsClient(const Napi::CallbackInfo& info);
    ~MmsClient();

private:
    static Napi::FunctionReference constructor;

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value ReadData(const Napi::CallbackInfo& info);
    Napi::Value Close(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
    static void connectionHandler(void* parameter, IedConnection connection, IedConnectionState state);

    IedConnection connection_;
    std::thread thread_;
    std::mutex connMutex_;
    Napi::ThreadSafeFunction tsfn_;
    bool running_;
    bool connected_;
    std::string clientID_;
};

#endif*/

#ifndef MMS_CLIENT_H
#define MMS_CLIENT_H

#include <napi.h>
//#include "hal_thread.h"
//#include "hal_time.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <iec61850_client.h>

class MmsClient : public Napi::ObjectWrap<MmsClient> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    MmsClient(const Napi::CallbackInfo& info);
    ~MmsClient();

private:
    static Napi::FunctionReference constructor;

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value ReadData(const Napi::CallbackInfo& info);
    Napi::Value Close(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
    Napi::Value GetLogicalDevices(const Napi::CallbackInfo& info);
    static void ConnectionHandler(void* parameter, IedConnection connection, IedConnectionState state);
    static void ConnectionIndicationHandler(void* parameter, IedConnection connection, IedConnectionState newState);
    Napi::Value ControlObject(const Napi::CallbackInfo& info);
    Napi::Value ReadDataSetValues(const Napi::CallbackInfo& info);
    Napi::Value CreateDataSet(const Napi::CallbackInfo& info);
    Napi::Value DeleteDataSet(const Napi::CallbackInfo& info);
    Napi::Value GetDataSetDirectory(const Napi::CallbackInfo& info);
    Napi::Value BrowseDataModel(const Napi::CallbackInfo& info);

    Napi::Value EnableReporting(const Napi::CallbackInfo& info);
    Napi::Value DisableReporting(const Napi::CallbackInfo& info);

    static void ReportCallback(void* parameter, ClientReport report);
   
    // Struct for holding MMS value data
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

    struct ReportInfo {
        ClientReportControlBlock rcb;
        ClientDataSet dataSet;
        LinkedList dataSetDirectory;
        std::string rcbRef;
    };
    std::map<std::string, ReportInfo> activeReports_;

    IedConnection connection_;
    std::thread thread_;
    std::mutex connMutex_;
    Napi::ThreadSafeFunction tsfn_;
    bool running_;
    bool connected_;
    std::string clientID_;
    bool usingPrimaryIp_;
};

#endif