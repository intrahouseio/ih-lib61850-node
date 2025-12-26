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

// Структура для хранения информации о именах элементов структуры
struct StructureElementNames {
    std::string ref;  // Полная ссылка на структуру
    FunctionalConstraint fc;
    std::vector<std::string> elementNames;
    std::vector<MmsType> elementTypes;
};

// Структура для кэша DataSet
struct DataSetCache {
    std::string datasetRef;
    std::vector<std::string> memberRefs;
    std::unordered_map<std::string, StructureElementNames> structureCache;
};

class MmsClient : public Napi::ObjectWrap<MmsClient> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    MmsClient(const Napi::CallbackInfo& info);
    ~MmsClient(); 
    std::recursive_mutex& GetMutex() { return connMutex_; }   

    // Struct for holding MMS value data
    /*struct ResultData {
        MmsType type;
        float floatValue;
        int64_t intValue;  
        bool boolValue;
        std::string stringValue;
        std::vector<ResultData> structureElements;
        std::vector<ResultData> arrayElements;
        bool isValid;
        std::string errorReason;
    };*/

    struct ResultData {
    MmsType type;
    bool isValid;
    std::string errorReason;
    
    // Для простых типов
    double floatValue;
    int64_t intValue;
    bool boolValue;
    std::string stringValue;
    
    // Для сложных типов
    std::vector<ResultData> structureElements;
    std::vector<ResultData> arrayElements;

    // Хранит имена элементов структуры
    std::vector<std::string> structureElementNames;
};

        // Методы для работы с кэшем
    void CacheDataSetStructure(const std::string& datasetRef, 
                              const std::vector<std::string>& memberRefs);
    bool GetCachedElementNames(const std::string& ref, FunctionalConstraint fc,
                              std::vector<std::string>& elementNames);
    void CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
                               const std::vector<std::string>& elementNames,
                               const std::vector<MmsType>& elementTypes);

private:
    static Napi::FunctionReference constructor;
    std::atomic<bool> isClosing_{false};
    void checkConnectionStatus();    
    std::atomic<bool> connectionCheckActive_{false};
    std::atomic<bool> disconnectEventSent_{false};

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value ReadData(const Napi::CallbackInfo& info);
    Napi::Value Close(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
    Napi::Value GetLogicalDevices(const Napi::CallbackInfo& info);
    static void ConnectionHandler(void* parameter, IedConnection connection, IedConnectionState state);
    static void ConnectionIndicationHandler(void* parameter, IedConnection connection, IedConnectionState newState);    
    static ResultData ConvertMmsValueToResultData(MmsValue* val, const std::string& attrName, IedConnection connection = nullptr, const std::string& parentRef = "", FunctionalConstraint fc = IEC61850_FC_ST);
    Napi::Value ControlObject(const Napi::CallbackInfo& info);
    Napi::Value ReadDataSetValues(const Napi::CallbackInfo& info);
    Napi::Value CreateDataSet(const Napi::CallbackInfo& info);
    Napi::Value DeleteDataSet(const Napi::CallbackInfo& info);
    Napi::Value GetDataSetDirectory(const Napi::CallbackInfo& info);
    Napi::Value BrowseDataModel(const Napi::CallbackInfo& info);

    Napi::Value EnableReporting(const Napi::CallbackInfo& info);
    Napi::Value DisableReporting(const Napi::CallbackInfo& info);     

    static void ReportCallback(void* parameter, ClientReport report);

     // Кэш для имен элементов структур
    std::unordered_map<std::string, DataSetCache> datasetCache_;
    
    // Методы для работы с кэшем
    //void CacheDataSetStructure(const std::string& datasetRef, 
    //                          const std::vector<std::string>& memberRefs);
    //bool GetCachedElementNames(const std::string& ref, FunctionalConstraint fc,
    //                          std::vector<std::string>& elementNames);
    //void CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
    //                           const std::vector<std::string>& elementNames,
    //                           const std::vector<MmsType>& elementTypes);
   
    


    struct ReportInfo {
        ClientReportControlBlock rcb;
        ClientDataSet dataSet;
        LinkedList dataSetDirectory;
        std::string rcbRef;
        std::string datasetRef;  
    };
    std::map<std::string, ReportInfo> activeReports_;

    IedConnection connection_;
    std::thread thread_;
    //std::mutex connMutex_;
    std::recursive_mutex connMutex_;
    Napi::ThreadSafeFunction tsfn_;
    bool running_;
    bool connected_;
    std::string clientID_;
    bool usingPrimaryIp_;    
    };

#endif