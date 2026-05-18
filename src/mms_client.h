#ifndef MMS_CLIENT_H
#define MMS_CLIENT_H

#include <napi.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <unordered_map>
#include <chrono>
#include <string>
#include <iec61850_client.h>
#include "mms_client_connection.h"

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

    Napi::Value ReadDataModel(const Napi::CallbackInfo& info);
    std::recursive_timed_mutex& GetMutex() { return connMutex_; }   

    // флаг для управления соединением
    bool connectionClosingIntentionally_ = false;

    std::unordered_map<std::string, DataSetCache>& GetDataSetCacheMutable() { return datasetCache_; }

    bool IsConnected() const { return connected_; }
    IedConnection GetConnection() const { return connection_; }

    struct ReportInfo {
        ClientReportControlBlock rcb = nullptr;
        ClientDataSet dataSet = nullptr;
        std::vector<std::string> dataSetMembers;
        std::string rcbRef;
        std::string datasetRef;
                
        // Добавляем кэш имен элементов для этого DataSet
        std::unordered_map<std::string, std::vector<std::string>> structureElementNamesCache;

        // НОВЫЙ: Кэш типов элементов структуры
        std::unordered_map<std::string, std::vector<MmsType>> structureElementTypesCache;

        // Диагностика
        size_t lastReportSize = 0;
        std::chrono::steady_clock::time_point lastReportTime;
        int reportsReceived = 0;
        
        // Конструктор по умолчанию
        ReportInfo() = default;
        
        // Конструктор копирования (исправлен порядок инициализации)
        ReportInfo(const ReportInfo& other) 
            : dataSetMembers(other.dataSetMembers),
              rcbRef(other.rcbRef),
              datasetRef(other.datasetRef),
              structureElementNamesCache(other.structureElementNamesCache),
              lastReportSize(other.lastReportSize),
              lastReportTime(other.lastReportTime),
              reportsReceived(other.reportsReceived) {
            // Указатели не копируем - они специфичны для каждого соединения
        }
        
        // Оператор присваивания
        ReportInfo& operator=(const ReportInfo& other) {
            if (this != &other) {
                rcbRef = other.rcbRef;
                datasetRef = other.datasetRef;
                dataSetMembers = other.dataSetMembers;
                structureElementNamesCache = other.structureElementNamesCache;
                lastReportSize = other.lastReportSize;
                lastReportTime = other.lastReportTime;
                reportsReceived = other.reportsReceived;
                // Указатели не копируем
            }
            return *this;
        }
        
        // Деструктор
        ~ReportInfo() {
            // Ресурсы освобождаются отдельно
        }
    };

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
    bool GetCachedElementNames(const std::string& fullRef, std::vector<std::string>& elementNames);

    void CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
                               const std::vector<std::string>& elementNames,
                               const std::vector<MmsType>& elementTypes);                               

    Napi::Value PollDataSetValues(const Napi::CallbackInfo& info);

        // Доступ к кэшу DataSet для воркеров
    const std::unordered_map<std::string, DataSetCache>& GetDataSetCache() const {
        return datasetCache_;
    }

private:
    // Диагностические счетчики
    static std::atomic<int> totalReportsProcessed_;
    static std::atomic<int> totalElementsProcessed_;
    static std::atomic<int> maxReportSize_;

    static Napi::FunctionReference constructor;
    bool isClosing_; 
    void CheckConnectionHealth();    
    std::atomic<bool> connectionCheckActive_{false};
    std::atomic<bool> disconnectEventSent_{false};

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value ReadData(const Napi::CallbackInfo& info);
    Napi::Value Close(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
    Napi::Value GetLogicalDevices(const Napi::CallbackInfo& info);
    static void ConnectionHandler(void* parameter, IedConnection connection, IedConnectionState state);
    static void ConnectionIndicationHandler(void* parameter, IedConnection connection, IedConnectionState newState);    
    Napi::Value ControlObject(const Napi::CallbackInfo& info);
    Napi::Value ReadDataSetModel(const Napi::CallbackInfo& info);
    Napi::Value CreateDataSet(const Napi::CallbackInfo& info);
    Napi::Value DeleteDataSet(const Napi::CallbackInfo& info);
    Napi::Value GetDataSetDirectory(const Napi::CallbackInfo& info);
    Napi::Value BrowseDataModel(const Napi::CallbackInfo& info);

    Napi::Value EnableReporting(const Napi::CallbackInfo& info);
    Napi::Value DisableReporting(const Napi::CallbackInfo& info);    

    static void ReportCallback(void* parameter, ClientReport report);

    Napi::Value ReadDataSetValuesFast(const std::string& datasetRef, Napi::Env env);    

    // Глобальный кэш для структур, не привязанных к конкретному DataSet
    std::unordered_map<std::string, StructureElementNames> globalStructureCache_;

    // Кэш для имен элементов структур
    std::unordered_map<std::string, DataSetCache> datasetCache_;
    
    std::map<std::string, ReportInfo> activeReports_;

    IedConnection connection_;
    std::thread thread_;
    //std::recursive_mutex connMutex_;
    std::recursive_timed_mutex connMutex_;
    Napi::ThreadSafeFunction tsfn_;
    bool running_;
    bool connected_;
    std::string clientID_;
    bool usingPrimaryIp_;

    // Новые методы для обхода модели данных
    Napi::Value GetRootNodes(Napi::Env env);
    Napi::Value BrowseSpecificObject(Napi::Env env, const std::string& ref);
    Napi::Value GetLogicalNodeDetails(Napi::Env env, const std::string& lnRef);
    Napi::Value GetDataObjectDetails(Napi::Env env, const std::string& doRef);
    Napi::Value GetDataSetDetails(Napi::Env env, const std::string& dsRef);
    Napi::Value GetReportDetails(Napi::Env env, const std::string& rRef);   

    // Вспомогательные методы для работы с кэшем
    void AddToStructureCache(const std::string& fullRef, 
                             const std::vector<std::string>& elementNames,
                             const std::vector<MmsType>& elementTypes);    
};

#endif