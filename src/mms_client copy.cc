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
#include <cstdint>
#include <atomic>
//#include <sys/resource.h>


static Napi::Value ProcessStructureWithCache(Napi::Env env, MmsClient* client,
                                            const std::string& fullRef, 
                                            MmsValue* structVal,
                                            int recursionDepth);

static Napi::Value ProcessMmsValueWithCache(Napi::Env env, MmsClient* client,
                                           const std::string& elementRef, 
                                           MmsValue* val, const std::string& elementName,
                                           int recursionDepth);

static Napi::Value SafeConvertMmsValue(Napi::Env env, IedConnection connection,
                                      MmsClient* client, 
                                      const std::string& elementRef, 
                                      MmsValue* val, const std::string& elementName,
                                      int recursionDepth = 0);
static MmsClient::ResultData ConvertMmsValueForReportFast(MmsValue* val, const std::string& attrName, int depth);
static void EnhanceResultDataWithCachedNames(MmsClient* client, MmsClient::ResultData& data, const std::string& fullRef, int depth);
static Napi::Value ResultDataToNapiWithNames(Napi::Env env, const MmsClient::ResultData& data, const std::string& attrName);
static FunctionalConstraint ParseFCFromString(const std::string& fcStr);

// Инициализация статических переменных класса MmsClient
std::atomic<int> MmsClient::totalReportsProcessed_(0);
std::atomic<int> MmsClient::totalElementsProcessed_(0);
std::atomic<int> MmsClient::maxReportSize_(0);

// Структура для хранения информации о элементе структуры
struct ElementInfo {
    std::string name;
    std::string fullRef;
    FunctionalConstraint fc;
};

namespace {
    // Структура для хранения результатов асинхронного чтения
    struct DataSetReadResult {
        std::string datasetRef;
        bool isValid;
        std::string errorReason;
        bool isDeletable;
        std::vector<std::string> memberRefs;
        std::vector<MmsClient::ResultData> values;
        int count;
    };
    
    // Асинхронный воркер для ReadDataSetModel
    class ReadDataSetModelWorker : public Napi::AsyncWorker {
    public:
        ReadDataSetModelWorker(MmsClient* client,
                            IedConnection connection,
                            std::recursive_mutex& connMutex,
                            Napi::Env env,
                            const std::vector<std::string>& datasetRefs,
                            Napi::Promise::Deferred deferred)
            : Napi::AsyncWorker(env),
            client_(client),
            connection_(connection),
            connMutex_(connMutex),
            env_(env),
            datasetRefs_(datasetRefs),
            deferred_(deferred) {}

        ~ReadDataSetModelWorker() {}

        void Execute() override {
            for (const auto& dsRef : datasetRefs_) {
                DataSetReadResult res;
                res.datasetRef = dsRef;
                res.isValid = false;

                // --- Шаг 1: Получение директории DataSet и memberRefs (под мьютексом) ---
                std::vector<std::string> memberRefs;
                bool isDeletable = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(connMutex_);
                    IedClientError error;
                    LinkedList members = IedConnection_getDataSetDirectory(
                        connection_, &error, dsRef.c_str(), &isDeletable);

                    if (error != IED_ERROR_OK || !members) {
                        res.errorReason = "Cannot get dataset directory, error: " + std::to_string(error);
                        results_.push_back(res);
                        continue;
                    }

                    LinkedList entry = members;
                    while (entry) {
                        if (entry->data) {
                            char* memberRef = (char*)entry->data;
                            memberRefs.push_back(std::string(memberRef));
                        }
                        entry = LinkedList_getNext(entry);
                    }
                    LinkedList_destroy(members);

                    // Кэшируем имена членов DataSet
                    client_->CacheDataSetStructure(dsRef, memberRefs);
                }

                res.memberRefs = memberRefs;

                // --- Шаг 2: Чтение значений DataSet (БЕЗ мьютекса) ---
                IedClientError error;
                ClientDataSet clientDataSet = nullptr;
                clientDataSet = IedConnection_readDataSetValues(
                    connection_, &error, dsRef.c_str(), nullptr);

                if (error != IED_ERROR_OK || !clientDataSet) {
                    res.errorReason = "Cannot read dataset values, error: " + std::to_string(error);
                    results_.push_back(res);
                    continue;
                }

                MmsValue* valuesArray = ClientDataSet_getValues(clientDataSet);
                if (!valuesArray || MmsValue_getType(valuesArray) != MMS_ARRAY) {
                    res.errorReason = "Invalid dataset values format";
                    ClientDataSet_destroy(clientDataSet);
                    results_.push_back(res);
                    continue;
                }

                int arraySize = MmsValue_getArraySize(valuesArray);
                int elementsToProcess = std::min(arraySize, (int)memberRefs.size());
                res.isValid = true;
                res.isDeletable = isDeletable;
                res.count = elementsToProcess;

                // Конвертация значений
                std::vector<MmsClient::ResultData> rawValues;
                for (int i = 0; i < elementsToProcess; ++i) {
                    MmsValue* val = MmsValue_getElement(valuesArray, i);
                    if (!val) continue;

                    const std::string& fullRef = memberRefs[i];
                    std::string attrName = fullRef;
                    size_t lastDot = fullRef.rfind('.');
                    if (lastDot != std::string::npos) attrName = fullRef.substr(lastDot + 1);

                    MmsClient::ResultData rd = ConvertMmsValueForReportFast(val, attrName, 0);
                    rawValues.push_back(rd);
                }

                // --- Шаг 3: Применение кэшированных имён структур (снова под мьютексом) ---
                {
                    std::lock_guard<std::recursive_mutex> lock(connMutex_);
                    for (size_t i = 0; i < rawValues.size(); ++i) {
                        const std::string& fullRef = memberRefs[i];
                        if (rawValues[i].type == MMS_STRUCTURE) {
                            EnhanceResultDataWithCachedNames(client_, rawValues[i], fullRef, 0);
                        }
                    }
                }

                res.values = std::move(rawValues);
                ClientDataSet_destroy(clientDataSet);
                results_.push_back(res);
            }
        }

        void OnOK() override {
            Napi::Env env = env_;
            Napi::Array resultArray = Napi::Array::New(env, results_.size());

            for (size_t idx = 0; idx < results_.size(); ++idx) {
                DataSetReadResult& res = results_[idx];
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("datasetRef", Napi::String::New(env, res.datasetRef));
                obj.Set("isValid", Napi::Boolean::New(env, res.isValid));

                if (!res.isValid) {
                    obj.Set("errorReason", Napi::String::New(env, res.errorReason));
                    resultArray.Set(idx, obj);
                    continue;
                }

                obj.Set("isDeletable", Napi::Boolean::New(env, res.isDeletable));
                obj.Set("count", Napi::Number::New(env, res.count));

                Napi::Object valuesObj = Napi::Object::New(env);
                Napi::Object memberRefsObj = Napi::Object::New(env);

                for (size_t i = 0; i < res.values.size(); ++i) {
                    const MmsClient::ResultData& rd = res.values[i];
                    const std::string& fullRef = res.memberRefs[i];
                    Napi::Value jsValue = ResultDataToNapiWithNames(env, rd, fullRef);
                    valuesObj.Set(fullRef, jsValue);
                    memberRefsObj.Set(fullRef, Napi::String::New(env, fullRef));
                }

                obj.Set("values", valuesObj);
                obj.Set("memberRefs", memberRefsObj);
                resultArray.Set(idx, obj);
            }

            deferred_.Resolve(resultArray);
        }

        void OnError(const Napi::Error& e) override {
            deferred_.Reject(e.Value());
        }

    private:
        MmsClient* client_;
        IedConnection connection_;
        std::recursive_mutex& connMutex_;
        Napi::Env env_;
        std::vector<std::string> datasetRefs_;
        Napi::Promise::Deferred deferred_;
        std::vector<DataSetReadResult> results_;
    };

    // Результат быстрого чтения одного DataSet (poll)
    struct DataSetPollResult {
        std::string datasetRef;
        bool isValid = false;
        std::string errorReason;
        std::vector<MmsClient::ResultData> values;
        std::vector<std::string> memberRefs;   // для восстановления имён
        int count = 0;
        uint64_t readTimeMicros = 0;
        uint64_t processTimeMicros = 0;
    };
    
    // Асинхронный воркер для PollDataSetValues
    class PollDataSetValuesWorker : public Napi::AsyncWorker {
    public:
        PollDataSetValuesWorker(MmsClient* client,
                                IedConnection connection,
                                std::recursive_mutex& connMutex,
                                Napi::Env env,
                                const std::vector<std::string>& datasetRefs,
                                Napi::Promise::Deferred deferred)
            : Napi::AsyncWorker(env),
            client_(client),
            connection_(connection),
            connMutex_(connMutex),
            env_(env),
            datasetRefs_(datasetRefs),
            deferred_(deferred) {}

        ~PollDataSetValuesWorker() {}

        void Execute() override {
            for (const auto& dsRef : datasetRefs_) {
                DataSetPollResult res;
                res.datasetRef = dsRef;
                res.isValid = false;

                // --- Шаг 1: Получение memberRefs из кэша под мьютексом ---
                std::vector<std::string> memberRefs;
                {
                    std::lock_guard<std::recursive_mutex> lock(connMutex_);
                    auto cacheIt = client_->GetDataSetCache().find(dsRef);
                    if (cacheIt == client_->GetDataSetCache().end()) {
                        res.errorReason = "DataSet not cached";
                        results_.push_back(res);
                        continue;
                    }
                    memberRefs = cacheIt->second.memberRefs; // копируем данные
                }

                // --- Шаг 2: Чтение DataSet (БЕЗ мьютекса) ---
                IedClientError error;
                ClientDataSet clientDataSet = nullptr;
                auto readStart = std::chrono::steady_clock::now();
                clientDataSet = IedConnection_readDataSetValues(
                    connection_, &error, dsRef.c_str(), nullptr);
                auto readEnd = std::chrono::steady_clock::now();
                res.readTimeMicros = std::chrono::duration_cast<std::chrono::microseconds>(readEnd - readStart).count();

                if (error != IED_ERROR_OK || !clientDataSet) {
                    res.errorReason = "Cannot read dataset values, error: " + std::to_string(error);
                    results_.push_back(res);
                    continue;
                }

                MmsValue* valuesArray = ClientDataSet_getValues(clientDataSet);
                if (!valuesArray || MmsValue_getType(valuesArray) != MMS_ARRAY) {
                    res.errorReason = "Invalid dataset values format";
                    ClientDataSet_destroy(clientDataSet);
                    results_.push_back(res);
                    continue;
                }

                int arraySize = MmsValue_getArraySize(valuesArray);
                int elementsToProcess = std::min(arraySize, (int)memberRefs.size());

                auto processStart = std::chrono::steady_clock::now();

                // Вектор для сырых ResultData (без имён структур)
                std::vector<MmsClient::ResultData> rawValues;
                rawValues.reserve(elementsToProcess);

                for (int i = 0; i < elementsToProcess; ++i) {
                    MmsValue* val = MmsValue_getElement(valuesArray, i);
                    if (!val) continue;

                    const std::string& fullRef = memberRefs[i];
                    std::string attrName = fullRef;
                    size_t lastDot = fullRef.rfind('.');
                    if (lastDot != std::string::npos) attrName = fullRef.substr(lastDot + 1);

                    MmsClient::ResultData rd = ConvertMmsValueForReportFast(val, attrName, 0);
                    rawValues.push_back(rd);
                }

                auto processEnd = std::chrono::steady_clock::now();
                res.processTimeMicros = std::chrono::duration_cast<std::chrono::microseconds>(processEnd - processStart).count();

                // --- Шаг 3: Применение кэшированных имён структур (снова под мьютексом) ---
                {
                    std::lock_guard<std::recursive_mutex> lock(connMutex_);
                    for (size_t i = 0; i < rawValues.size(); ++i) {
                        const std::string& fullRef = memberRefs[i];
                        if (rawValues[i].type == MMS_STRUCTURE) {
                            EnhanceResultDataWithCachedNames(client_, rawValues[i], fullRef, 0);
                        }
                    }
                }

                res.values = std::move(rawValues);
                res.isValid = true;
                res.count = elementsToProcess;
                res.memberRefs = std::move(memberRefs); // сохраняем для последующего использования в OnOK

                ClientDataSet_destroy(clientDataSet);
                results_.push_back(res);
            }
        }

        void OnOK() override {
            Napi::Env env = env_;
            Napi::Array resultArray = Napi::Array::New(env, results_.size());

            for (size_t idx = 0; idx < results_.size(); ++idx) {
                DataSetPollResult& res = results_[idx];
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("datasetRef", Napi::String::New(env, res.datasetRef));
                obj.Set("isValid", Napi::Boolean::New(env, res.isValid));

                if (!res.isValid) {
                    obj.Set("errorReason", Napi::String::New(env, res.errorReason));
                    resultArray.Set(idx, obj);
                    continue;
                }

                obj.Set("count", Napi::Number::New(env, res.count));
                obj.Set("readTimeMicros", Napi::Number::New(env, static_cast<double>(res.readTimeMicros)));
                obj.Set("processTimeMicros", Napi::Number::New(env, static_cast<double>(res.processTimeMicros)));

                Napi::Object valuesObj = Napi::Object::New(env);
                for (size_t i = 0; i < res.values.size(); ++i) {
                    const MmsClient::ResultData& rd = res.values[i];
                    const std::string& fullRef = res.memberRefs[i];
                    Napi::Value jsValue = ResultDataToNapiWithNames(env, rd, fullRef);
                    valuesObj.Set(fullRef, jsValue);
                }
                obj.Set("values", valuesObj);
                resultArray.Set(idx, obj);
            }

            deferred_.Resolve(resultArray);
        }

        void OnError(const Napi::Error& e) override {
            deferred_.Reject(e.Value());
        }

    private:
        MmsClient* client_;
        IedConnection connection_;
        std::recursive_mutex& connMutex_;
        Napi::Env env_;
        std::vector<std::string> datasetRefs_;
        Napi::Promise::Deferred deferred_;
        std::vector<DataSetPollResult> results_;
    };
} // анонимный namespace

Napi::FunctionReference MmsClient::constructor;

struct ConnectionHandlerContext {
    MmsClient* client;
    std::recursive_mutex* mutex;  
};

void MmsClient::CheckConnectionHealth() {
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    if (!connected_ || !connection_) {
        return;
    }
    
    // Проверяем состояние соединения
    IedConnectionState state = IedConnection_getState(connection_);
    //printf("[Health Check] Connection state: %d\n", state);
    
    // Если соединение активно более 30 секунд, отправляем тестовый запрос
    static auto lastHealthCheckTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastCheck = std::chrono::duration_cast<std::chrono::seconds>(now - lastHealthCheckTime);
    
    if (timeSinceLastCheck.count() > 30) {
        //printf("[Health Check] Sending test request to keep connection alive...\n");
        lastHealthCheckTime = now;
        
        // Попробуем прочитать простой атрибут для поддержания активности
        IedClientError error;
        MmsValue* value = IedConnection_readObject(connection_, &error, 
            "WAGO61850ServerDevice/LLN0.Beh[ST]", IEC61850_FC_ST);
        
        if (error == IED_ERROR_OK && value) {
            //printf("[Health Check] Test request successful\n");
            MmsValue_delete(value);
        } else {
            //printf("[Health Check] Test request failed, error: %d\n", error);
        }
    }
}

static void LogNetworkErrorDetailed(IedClientError error) {
    //printf("[Network Error] Code: %d, Description: ", error);
    
    switch (error) {
        case IED_ERROR_OK: 
            printf("No error"); 
            break;
        case IED_ERROR_NOT_CONNECTED: 
            printf("Not connected"); 
            break;
        case IED_ERROR_CONNECTION_LOST: 
            printf("Connection lost"); 
            break;
        case IED_ERROR_SERVICE_NOT_SUPPORTED: 
            printf("Service not supported"); 
            break;
        case IED_ERROR_CONNECTION_REJECTED: 
            printf("Connection rejected"); 
            break;
        case IED_ERROR_ACCESS_DENIED: 
            printf("Access denied"); 
            break;        
        case IED_ERROR_OBJECT_ACCESS_UNSUPPORTED: 
            printf("Object access unsupported"); 
            break;       
        case IED_ERROR_OBJECT_VALUE_INVALID: 
            printf("Object value invalid"); 
            break;
        case IED_ERROR_TYPE_INCONSISTENT: 
            printf("Type inconsistent"); 
            break;
        case IED_ERROR_TIMEOUT: 
            printf("Timeout"); 
            break;
        case IED_ERROR_OUTSTANDING_CALL_LIMIT_REACHED: 
            printf("Outstanding call limit exceeded - too many concurrent requests");
            break;        
        case IED_ERROR_UNKNOWN:
            printf("Unknown error");
            break;
        default: 
            printf("Unknown error code: %d", error);
    }
    printf("\n");
    
    // Добавляем время ошибки
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    //printf("[Network Error] Time: %s", std::ctime(&time_t_now));
}

// Упрощенная структура для хранения имен элементов (без типов)
struct SimpleStructureInfo {
    std::vector<std::string> elementNames;
};

// Быстрая функция для получения имен элементов из кэша отчета
static bool GetReportCachedElementNames(MmsClient::ReportInfo& reportInfo,
                                       const std::string& ref,
                                       FunctionalConstraint fc,
                                       std::vector<std::string>& elementNames,
                                       std::vector<MmsType>* elementTypes = nullptr) {  // Добавлен параметр для типов
    
    // Формируем ключ
    std::string fcStr;
    if (fc == IEC61850_FC_ST) fcStr = "ST";
    else if (fc == IEC61850_FC_MX) fcStr = "MX";
    else if (fc == IEC61850_FC_CO) fcStr = "CO";
    else if (fc == IEC61850_FC_CF) fcStr = "CF";
    else if (fc == IEC61850_FC_DC) fcStr = "DC";
    else if (fc == IEC61850_FC_SP) fcStr = "SP";
    else if (fc == IEC61850_FC_SG) fcStr = "SG";
    else if (fc == IEC61850_FC_BR) fcStr = "BR";
    else if (fc == IEC61850_FC_RP) fcStr = "RP";
    else if (fc == IEC61850_FC_EX) fcStr = "EX";
    else if (fc == IEC61850_FC_SR) fcStr = "SR";
    else if (fc == IEC61850_FC_OR) fcStr = "OR";
    else if (fc == IEC61850_FC_BL) fcStr = "BL";
    else if (fc == IEC61850_FC_LG) fcStr = "LG";
    else if (fc == IEC61850_FC_GO) fcStr = "GO";
    else if (fc == IEC61850_FC_MS) fcStr = "MS";
    else if (fc == IEC61850_FC_US) fcStr = "US";
    else if (fc == IEC61850_FC_ALL) fcStr = "ALL";
    else fcStr = std::to_string(fc);
    
    std::string cacheKey = ref + "[" + fcStr + "]";
    
    auto it = reportInfo.structureElementNamesCache.find(cacheKey);
    if (it != reportInfo.structureElementNamesCache.end()) {
        elementNames = it->second;
        
        // Если запрошены типы, возвращаем их тоже
        if (elementTypes) {
            auto typeIt = reportInfo.structureElementTypesCache.find(cacheKey);
            if (typeIt != reportInfo.structureElementTypesCache.end()) {
                *elementTypes = typeIt->second;
            }
        }
        return true;
    }
    
    // Пробуем найти без FC
    for (const auto& [key, names] : reportInfo.structureElementNamesCache) {
        if (key.find(ref) == 0) {  // Начинается с ref
            elementNames = names;
            
            // Если запрошены типы, возвращаем их тоже
            if (elementTypes) {
                auto typeIt = reportInfo.structureElementTypesCache.find(key);
                if (typeIt != reportInfo.structureElementTypesCache.end()) {
                    *elementTypes = typeIt->second;
                }
            }
            return true;
        }
    }
    
    return false;
}

// Функция для улучшения структуры с использованием кэшированных имен
static void EnhanceStructureWithCachedNames(MmsClient::ResultData& data,
                                          const std::string& fullRef,
                                          MmsClient::ReportInfo& reportInfo,
                                          int depth = 0) {
    
    if (data.type != MMS_STRUCTURE || data.structureElements.empty()) {
        return;
    }
    
    const int MAX_RECURSION_DEPTH = 5;
    if (depth > MAX_RECURSION_DEPTH) {
        return;
    }
    
    // Извлекаем FC и чистую ссылку
    std::string cleanRef = fullRef;
    FunctionalConstraint fc = IEC61850_FC_ST;
    
    size_t bracketPos = fullRef.find('[');
    if (bracketPos != std::string::npos && fullRef.back() == ']') {
        std::string fcStr = fullRef.substr(bracketPos + 1, fullRef.length() - bracketPos - 2);
        cleanRef = fullRef.substr(0, bracketPos);
        // Преобразуем строку FC в число
        if (fcStr == "ST") fc = IEC61850_FC_ST;
        else if (fcStr == "MX") fc = IEC61850_FC_MX;
        else if (fcStr == "CO") fc = IEC61850_FC_CO;
        else if (fcStr == "CF") fc = IEC61850_FC_CF;
        else if (fcStr == "DC") fc = IEC61850_FC_DC;
        else if (fcStr == "SP") fc = IEC61850_FC_SP;
        else if (fcStr == "SG") fc = IEC61850_FC_SG;
        else if (fcStr == "BR") fc = IEC61850_FC_BR;
        else if (fcStr == "RP") fc = IEC61850_FC_RP;
        else if (fcStr == "EX") fc = IEC61850_FC_EX;
        else if (fcStr == "SR") fc = IEC61850_FC_SR;
        else if (fcStr == "OR") fc = IEC61850_FC_OR;
        else if (fcStr == "BL") fc = IEC61850_FC_BL;
        else if (fcStr == "LG") fc = IEC61850_FC_LG;
        else if (fcStr == "GO") fc = IEC61850_FC_GO;
        else if (fcStr == "MS") fc = IEC61850_FC_MS;
        else if (fcStr == "US") fc = IEC61850_FC_US;
        else if (fcStr == "ALL") fc = IEC61850_FC_ALL;
    }
    
    // Получаем кэшированные имена для этой структуры
    std::vector<std::string> elementNames;
    bool hasCachedNames = GetReportCachedElementNames(reportInfo, cleanRef, fc, elementNames);
    
    if (hasCachedNames && elementNames.size() == data.structureElements.size()) {
        // Заменяем временные числовые имена на реальные
        data.structureElementNames = elementNames;
        
        // Рекурсивно улучшаем вложенные структуры
        for (size_t i = 0; i < data.structureElements.size(); ++i) {
            if (data.structureElements[i].type == MMS_STRUCTURE) {
                std::string childRef = cleanRef + "." + elementNames[i];
                if (bracketPos != std::string::npos) {
                    std::string fcPart = fullRef.substr(bracketPos);
                    childRef += fcPart;
                }
                EnhanceStructureWithCachedNames(data.structureElements[i], childRef, reportInfo, depth + 1);
            }
        }
    }
}

// Новая рекурсивная функция для кэширования имен элементов структуры
static void CacheStructureElementNames(IedConnection connection,
                                     MmsClient* client,
                                     MmsClient::ReportInfo& reportInfo,
                                     const std::string& baseRef,
                                     FunctionalConstraint fc,
                                     int recursionDepth = 0) {
    
    const int MAX_CACHE_DEPTH = 5;
    if (recursionDepth > MAX_CACHE_DEPTH) {
        //printf("    [NameCache-%d] Max cache depth reached for %s\n", recursionDepth, baseRef.c_str());
        return;
    }
    
    //printf("    [NameCache-%d] Caching structure names for: %s (FC=%d)\n", recursionDepth, baseRef.c_str(), fc);
    
    // Создаем ключ для кэша
    std::string fcStr;
    if (fc == IEC61850_FC_ST) fcStr = "ST";
    else if (fc == IEC61850_FC_MX) fcStr = "MX";
    else if (fc == IEC61850_FC_CO) fcStr = "CO";
    else if (fc == IEC61850_FC_CF) fcStr = "CF";
    else if (fc == IEC61850_FC_DC) fcStr = "DC";
    else if (fc == IEC61850_FC_SP) fcStr = "SP";
    else if (fc == IEC61850_FC_SG) fcStr = "SG";
    else if (fc == IEC61850_FC_BR) fcStr = "BR";
    else if (fc == IEC61850_FC_RP) fcStr = "RP";
    else if (fc == IEC61850_FC_EX) fcStr = "EX";
    else if (fc == IEC61850_FC_SR) fcStr = "SR";
    else if (fc == IEC61850_FC_OR) fcStr = "OR";
    else if (fc == IEC61850_FC_BL) fcStr = "BL";
    else if (fc == IEC61850_FC_LG) fcStr = "LG";
    else if (fc == IEC61850_FC_GO) fcStr = "GO";
    else if (fc == IEC61850_FC_MS) fcStr = "MS";
    else if (fc == IEC61850_FC_US) fcStr = "US";
    else if (fc == IEC61850_FC_ALL) fcStr = "ALL";
    else fcStr = std::to_string(fc);
    
    std::string cacheKey = baseRef + "[" + fcStr + "]";
    
    // Проверяем, не кэшировали ли уже
    if (reportInfo.structureElementNamesCache.find(cacheKey) != 
        reportInfo.structureElementNamesCache.end()) {
        //printf("    [NameCache-%d] Already cached: %s\n", recursionDepth, cacheKey.c_str());
        return;
    }
    
    // Получаем спецификацию
    IedClientError error;
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(
        connection, &error, baseRef.c_str(), fc);
    
    if (error != IED_ERROR_OK || spec == nullptr) {
        //printf("    [NameCache-%d] FAILED to get var spec for %s, error: %d\n", recursionDepth, baseRef.c_str(), error);
        return;
    }
    
    int type = MmsVariableSpecification_getType(spec);
    
    if (type == MMS_STRUCTURE) {
        int size = MmsVariableSpecification_getSize(spec);
        //printf("    [NameCache-%d] Structure size: %d\n", recursionDepth, size);
        
        std::vector<std::string> elementNames;
        
        for (int i = 0; i < size; i++) {
            MmsVariableSpecification* childSpec = 
                MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
            
            if (childSpec != nullptr) {
                const char* name = MmsVariableSpecification_getName(childSpec);
                MmsType childType = static_cast<MmsType>(
                    MmsVariableSpecification_getType(childSpec));
                
                if (name != nullptr) {
                    elementNames.push_back(std::string(name));
                    //printf("    [NameCache-%d]   Child %d: %s, type=%d\n", recursionDepth, i, name, childType);
                    
                    // Рекурсивно кэшируем вложенные структуры
                    if (childType == MMS_STRUCTURE) {
                        std::string childRef = baseRef + "." + name;
                        CacheStructureElementNames(connection, client, reportInfo, 
                                                 childRef, fc, recursionDepth + 1);
                    }
                }
            }
        }
        
        // Сохраняем в кэш отчета
        reportInfo.structureElementNamesCache[cacheKey] = elementNames;
        //printf("    [NameCache-%d] Cached %zu element names for %s\n", recursionDepth, elementNames.size(), cacheKey.c_str());
    }
    
    MmsVariableSpecification_destroy(spec);
}

// Упрощенная функция для быстрой конвертации MMS значений
static MmsClient::ResultData ConvertMmsValueForReportFast(MmsValue* val, const std::string& attrName, int depth = 0) {
    //printf("    ConvertMmsValueForReportFast: attrName='%s', MMS type=%d\n", attrName.c_str(), MmsValue_getType(val));

    MmsClient::ResultData data;
    
    // Ограничиваем глубину рекурсии
    const int MAX_RECURSION_DEPTH = 5;
    if (depth > MAX_RECURSION_DEPTH) {
        data.type = MMS_STRUCTURE;
        data.isValid = false;
        data.errorReason = "Max recursion depth exceeded";
        return data;
    }
    
    if (!val) {
        data.type = MMS_DATA_ACCESS_ERROR;
        data.isValid = false;
        data.errorReason = "Null value";
        return data;
    }

    data.type = MmsValue_getType(val);
    data.isValid = true;
    data.errorReason = "";

    if (data.type < 0 || data.type > 14) {
        data.isValid = false;
        data.errorReason = "Unsupported MMS type: " + std::to_string(data.type);
        return data;
    }

    try {
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
                
                // Специальная обработка для stVal типа DPC, который в отчетах приходит как INTEGER
                if (attrName.find("stVal") != std::string::npos) {
                    // Преобразование согласно стандарту для DPC:
                    // 0 = intermediate-state, 1 = off, 2 = on, 3 = bad-state
                    int64_t intVal = data.intValue;
                    switch (intVal) {
                        case 0: data.stringValue = "intermediate-state"; break;
                        case 1: data.stringValue = "off"; break;
                        case 2: data.stringValue = "on"; break;
                        case 3: data.stringValue = "bad-state"; break;
                        default: data.stringValue = "unknown(" + std::to_string(intVal) + ")";
                    }
                    // Для отладки:
                    //printf("    [DPC-stVal] attrName='%s', intValue=%lld, stringValue='%s'\n", attrName.c_str(), data.intValue, data.stringValue.c_str());
                }
                break;
            case MMS_BOOLEAN:
                data.boolValue = MmsValue_getBoolean(val);
                break;

            case MMS_VISIBLE_STRING: {
                const char* str = MmsValue_toString(val);
                data.stringValue = str ? str : "";
                break;
            }

            case MMS_UTC_TIME:
                data.intValue = static_cast<int64_t>(MmsValue_getUtcTimeInMs(val));
                break;

            case MMS_BIT_STRING: {
                uint32_t bits = MmsValue_getBitStringAsInteger(val);
                data.intValue = static_cast<int64_t>(bits);
                
                int bitSize = MmsValue_getBitStringSize(val);
                
                // 1. Общий отладочный вывод для всех битовых строк
                //printf("    [BitString] attrName='%s', bits=%u (0x%X), size=%d\n", attrName.c_str(), bits, bits, bitSize);
                
                // 2. ПРЯМОЕ СОПОСТАВЛЕНИЕ: если attrName ТОЧНО РАВЕН "stVal"
                if (attrName == "stVal" && bitSize == 2) {
                    //printf("    [Прямое сопоставление stVal] Найдено по имени.\n");
                    uint32_t msbValue = 0;
                    uint32_t lsbValue = bits;
                    for (int i = 0; i < 2; i++) {
                        int bit = (lsbValue >> i) & 1;
                        msbValue |= (bit << (1 - i));
                    }
                    data.intValue = static_cast<int64_t>(msbValue);
                    switch (msbValue) {
                        case 0: data.stringValue = "intermediate-state"; break;
                        case 1: data.stringValue = "off"; break;
                        case 2: data.stringValue = "on"; break;
                        case 3: data.stringValue = "bad-state"; break;
                        default: data.stringValue = "unknown(" + std::to_string(msbValue) + ")";
                    }
                    //printf("    [DPC] Преобразованное: intValue=%lld, stringValue='%s'\n", data.intValue, data.stringValue.c_str());
                }
                // 3. ЭВРИСТИКА ДЛЯ ОТЧЕТОВ: если имя - цифра, это может быть индекс внутри структуры
                //    Проверяем, является ли attrName одной цифрой (например, '0', '1').
                else if (attrName.length() == 1 && isdigit(attrName[0])) {
                    int index = attrName[0] - '0'; // Преобразуем символ цифры в число
                    
                    // Индекс 0 в структуре статуса [ST] - это stVal (DPC, 2 бита)
                    if (index == 0 && bitSize == 2) {
                        //printf("    [Эвристика] Обнаружен вероятный stVal по индексу 0 в структуре.\n");
                        uint32_t msbValue = 0;
                        uint32_t lsbValue = bits;
                        for (int i = 0; i < 2; i++) {
                            int bit = (lsbValue >> i) & 1;
                            msbValue |= (bit << (1 - i));
                        }
                        data.intValue = static_cast<int64_t>(msbValue);
                        switch (msbValue) {
                            case 0: data.stringValue = "intermediate-state"; break;
                            case 1: data.stringValue = "off"; break;
                            case 2: data.stringValue = "on"; break;
                            case 3: data.stringValue = "bad-state"; break;
                            default: data.stringValue = "unknown(" + std::to_string(msbValue) + ")";
                        }
                        //printf("    [DPC] Преобразованное: intValue=%lld, stringValue='%s'\n", data.intValue, data.stringValue.c_str());
                    }
                    // Индекс 1 - это качество 'q' (оставляем как битовую строку-число)
                    else if (index == 1) {
                        //printf("    [Эвристика] Обнаружено качество (q) по индексу 1. Значение: %u\n", bits);
                        // Для q оставляем data.intValue = bits (битовая строка как число)
                        // data.stringValue остаётся пустой
                    }
                }
                break;
            }

            case MMS_STRUCTURE: {
                int size = MmsValue_getArraySize(val);
                
                // Простая обработка структуры с числовыми индексами
                // Имена будут заменены позже с использованием кэша
                for (int i = 0; i < size; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        std::string elementName = std::to_string(i); // Временное имя
                        data.structureElements.push_back(
                            ConvertMmsValueForReportFast(el, elementName, depth + 1));
                    }
                }
                break;
            }

            case MMS_ARRAY: {
                int size = MmsValue_getArraySize(val);
                
                // Ограничиваем размер массива
                const int MAX_ARRAY_SIZE = 50;
                int elementsToProcess = std::min(size, MAX_ARRAY_SIZE);
                
                for (int i = 0; i < elementsToProcess; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        data.arrayElements.push_back(
                            ConvertMmsValueForReportFast(el, attrName, depth + 1));
                    }
                }
                break;
            }

            default:
                data.isValid = false;
                data.errorReason = "Unsupported MMS type: " + std::to_string(data.type);
                break;
        }
    } catch (const std::exception& e) {
        data.isValid = false;
        data.errorReason = std::string("Exception: ") + e.what();
    } catch (...) {
        data.isValid = false;
        data.errorReason = "Unknown exception";
    }
    
    return data;
}

// Функция для преобразования строки FC в числовое значение
static FunctionalConstraint ParseFCFromString(const std::string& fcStr) {
    // Сначала попробуем сопоставить с текстовыми обозначениями
    std::string upperFcStr = fcStr;
    std::transform(upperFcStr.begin(), upperFcStr.end(), upperFcStr.begin(), ::toupper);
    
    //printf("    ParseFCFromString: input='%s', upper='%s'\n", fcStr.c_str(), upperFcStr.c_str());

    if (upperFcStr == "ST" || upperFcStr == "0") return IEC61850_FC_ST;
    else if (upperFcStr == "MX" || upperFcStr == "1") return IEC61850_FC_MX;
    else if (upperFcStr == "CO" || upperFcStr == "2") return IEC61850_FC_CO;
    else if (upperFcStr == "CF" || upperFcStr == "3") return IEC61850_FC_CF;
    else if (upperFcStr == "DC" || upperFcStr == "4") return IEC61850_FC_DC;
    else if (upperFcStr == "SP" || upperFcStr == "5") return IEC61850_FC_SP;
    else if (upperFcStr == "SG" || upperFcStr == "6") return IEC61850_FC_SG;
    else if (upperFcStr == "BR" || upperFcStr == "7") return IEC61850_FC_BR;
    else if (upperFcStr == "RP" || upperFcStr == "8") return IEC61850_FC_RP;
    else if (upperFcStr == "EX" || upperFcStr == "9") return IEC61850_FC_EX;
    else if (upperFcStr == "SR" || upperFcStr == "10") return IEC61850_FC_SR;
    else if (upperFcStr == "OR" || upperFcStr == "11") return IEC61850_FC_OR;
    else if (upperFcStr == "BL" || upperFcStr == "12") return IEC61850_FC_BL;
    else if (upperFcStr == "LG" || upperFcStr == "13") return IEC61850_FC_LG;
    else if (upperFcStr == "GO" || upperFcStr == "14") return IEC61850_FC_GO;
    else if (upperFcStr == "MS" || upperFcStr == "15") return IEC61850_FC_MS;
    else if (upperFcStr == "US" || upperFcStr == "16") return IEC61850_FC_US;
    else if (upperFcStr == "ALL" || upperFcStr == "17") return IEC61850_FC_ALL;
    else {
        printf("    WARNING: Unknown FC string '%s', defaulting to ST\n", fcStr.c_str());
        return IEC61850_FC_ST;
    }
}

// Новая рекурсивная функция для кэширования всех уровней структуры
static void RecursiveCacheStructureElements(IedConnection connection,
                                          MmsClient* client,
                                          const std::string& baseRef,
                                          FunctionalConstraint fc,
                                          int recursionDepth = 0) {
    
    const int MAX_CACHE_DEPTH = 5;
    if (recursionDepth > MAX_CACHE_DEPTH) {
    //    printf("    [Cache-%d] Max cache depth reached for %s\n", recursionDepth, baseRef.c_str());
        return;
    }
    
    //printf("    [Cache-%d] START for: %s (FC=%d)\n", recursionDepth, baseRef.c_str(), fc);
    
    // Получаем спецификацию
    IedClientError error;
    //printf("    [Cache-%d] Calling IedConnection_getVariableSpecification...\n", recursionDepth);
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(connection, &error, baseRef.c_str(), fc);
    
    if (error != IED_ERROR_OK || spec == nullptr) {
    //    printf("    [Cache-%d] FAILED to get var spec for %s, error: %d\n", recursionDepth, baseRef.c_str(), error);
        return;
    }
    
    //printf("    [Cache-%d] Got var spec, type=%d\n", recursionDepth, MmsVariableSpecification_getType(spec));
    
    int type = MmsVariableSpecification_getType(spec);
    
    if (type == MMS_STRUCTURE) {
        int size = MmsVariableSpecification_getSize(spec);
        //printf("    [Cache-%d] Structure size: %d\n", recursionDepth, size);
        
        std::vector<std::string> elementNames;
        std::vector<MmsType> elementTypes;
        std::vector<std::pair<std::string, MmsType>> childInfo;
        
        for (int i = 0; i < size; i++) {
            //printf("    [Cache-%d] Processing child %d...\n", recursionDepth, i);
            
            MmsVariableSpecification* childSpec = 
                MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
            
            if (childSpec != nullptr) {
                const char* name = MmsVariableSpecification_getName(childSpec);
                MmsType childType = static_cast<MmsType>(
                    MmsVariableSpecification_getType(childSpec));
                
                if (name != nullptr) {
                    elementNames.push_back(std::string(name));
                    elementTypes.push_back(childType);
                    childInfo.push_back({std::string(name), childType});
                    
                    //printf("    [Cache-%d]   Child %d: %s, type=%d\n", recursionDepth, i, name, childType);
                } else {
                    //printf("    [Cache-%d]   Child %d: name is NULL\n", recursionDepth, i);
                }
            } else {
                //printf("    [Cache-%d]   Child %d: spec is NULL\n", recursionDepth, i);
            }
        }
        
        //printf("    [Cache-%d] Collected %zu children\n", recursionDepth, childInfo.size());
        
        // Кэшируем текущий уровень
       if (!elementNames.empty()) {
        // Преобразуем FC в строковое представление
        std::string fcStr;
        if (fc == IEC61850_FC_ST) fcStr = "ST";
        else if (fc == IEC61850_FC_MX) fcStr = "MX";
        else if (fc == IEC61850_FC_CO) fcStr = "CO";
        else if (fc == IEC61850_FC_CF) fcStr = "CF";
        else if (fc == IEC61850_FC_DC) fcStr = "DC";
        else if (fc == IEC61850_FC_SP) fcStr = "SP";
        else if (fc == IEC61850_FC_SG) fcStr = "SG";
        else if (fc == IEC61850_FC_BR) fcStr = "BR";
        else if (fc == IEC61850_FC_RP) fcStr = "RP";
        else if (fc == IEC61850_FC_EX) fcStr = "EX";
        else if (fc == IEC61850_FC_SR) fcStr = "SR";
        else if (fc == IEC61850_FC_OR) fcStr = "OR";
        else if (fc == IEC61850_FC_BL) fcStr = "BL";
        else if (fc == IEC61850_FC_LG) fcStr = "LG";
        else if (fc == IEC61850_FC_GO) fcStr = "GO";
        else if (fc == IEC61850_FC_MS) fcStr = "MS";
        else if (fc == IEC61850_FC_US) fcStr = "US";
        else if (fc == IEC61850_FC_ALL) fcStr = "ALL";
        else fcStr = std::to_string(fc);

        std::string refWithFc = baseRef + "[" + fcStr + "]";
        //printf("    [CacheStore] Storing with key: %s (FC=%d as '%s')\n", refWithFc.c_str(), fc, fcStr.c_str());
        
        // УБЕДИТЕСЬ, что этот вызов выполняется (не закомментирован):
        std::lock_guard<std::recursive_mutex> lock(client->GetMutex());           
        client->CacheStructureElements(refWithFc, fc, elementNames, elementTypes);
        //printf("    [Cache-%d] CacheStructureElements CALLED for %s\n", recursionDepth, refWithFc.c_str());
        }
        
        //printf("    [Cache-%d] Checking for recursive structures...\n", recursionDepth);
        // Рекурсивные вызовы
        for (const auto& [childName, childType] : childInfo) {
            if (childType == MMS_STRUCTURE) {
                std::string childRef = baseRef + "." + childName;
                //printf("    [Cache-%d] Recursing into: %s (type=%d)\n", recursionDepth, childRef.c_str(), childType);
                RecursiveCacheStructureElements(connection, client, 
                                               childRef, fc, 
                                               recursionDepth + 1);
            } else {
                //printf("    [Cache-%d] Child %s is not a structure (type=%d), skipping recursion\n", recursionDepth, childName.c_str(), childType);
            }
        }
        
    } else {
        //printf("    [Cache-%d] Not a structure, type=%d\n", recursionDepth, type);
    }
    
    //printf("    [Cache-%d] Destroying variable spec for %s...\n", recursionDepth, baseRef.c_str());
    MmsVariableSpecification_destroy(spec);
    //printf("    [Cache-%d] FINISHED processing %s\n\n", recursionDepth, baseRef.c_str());
}

void MmsClient::CacheDataSetStructure(const std::string& datasetRef, 
                                     const std::vector<std::string>& memberRefs) {
    std::lock_guard<std::recursive_mutex> lock(connMutex_);

    
    
    //printf("!!! DEBUG: ENTERING CacheDataSetStructure !!!\n");
    //printf("!!! DEBUG: datasetRef = %s\n", datasetRef.c_str());
    //printf("!!! DEBUG: connected_ = %d, connection_ = %p\n", connected_, (void*)connection_);
    fflush(stdout);

    if (!connected_ || !connection_) {
        printf("CacheDataSetStructure: Not connected, skipping cache\n");
        fflush(stdout);
        return;
    }
    
    //printf("CacheDataSetStructure called for: %s\n", datasetRef.c_str());
    //printf("Current cache size: %zu\n", datasetCache_.size());
    
    // ПРОВЕРКА: если DataSet уже закэширован, пропускаем кэширование
    if (datasetCache_.find(datasetRef) != datasetCache_.end()) {
        //printf("CacheDataSetStructure: DataSet %s already cached, skipping.\n", datasetRef.c_str());
        //printf("  Already cached members: %zu\n", datasetCache_[datasetRef].memberRefs.size());
        return;
    }
    
    printf("CacheDataSetStructure: DataSet %s not cached, caching %zu members...\n", 
           datasetRef.c_str(), memberRefs.size());
      
    DataSetCache cache;
    cache.datasetRef = datasetRef;
    cache.memberRefs = memberRefs;
    
    printf("\n=== Recursive caching for DataSet: %s ===\n", datasetRef.c_str());
    printf("Number of members to cache: %zu\n", memberRefs.size());
    fflush(stdout);
    
    // Обрабатываем каждый элемент DataSet рекурсивно
    for (const auto& memberRef : memberRefs) {
        //printf("\n  Processing member: %s\n", memberRef.c_str());
        
        // Извлекаем FC и чистую ссылку
        std::string cleanRef = memberRef;
        FunctionalConstraint fc = IEC61850_FC_ST;
        
        size_t bracketPos = memberRef.find('[');
        if (bracketPos != std::string::npos && memberRef.back() == ']') {
            std::string fcStr = memberRef.substr(bracketPos + 1, 
                                                memberRef.length() - bracketPos - 2);
            cleanRef = memberRef.substr(0, bracketPos);
            fc = ParseFCFromString(fcStr);
            //printf("    Extracted: cleanRef='%s', fcStr='%s', fc=%d\n", cleanRef.c_str(), fcStr.c_str(), fc);
        } else {
            //printf("    WARNING: No FC in memberRef '%s', using default ST\n", memberRef.c_str());
        }
        
        // Рекурсивно кэшируем все уровни структуры
        //printf("    Starting recursive cache for '%s' with FC=%d\n", cleanRef.c_str(), fc);
        RecursiveCacheStructureElements(connection_, this, cleanRef, fc, 0);
        //printf("    Finished recursive cache for '%s'\n", cleanRef.c_str());
    }
    
    datasetCache_[datasetRef] = cache;
    printf("\n=== Finished recursive caching for DataSet %s ===\n", datasetRef.c_str());
    printf("Total cached datasets: %zu\n", datasetCache_.size());
}

// Исправленный метод GetCachedElementNames
bool MmsClient::GetCachedElementNames(const std::string& ref, FunctionalConstraint fc,
                                     std::vector<std::string>& elementNames) {
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    //printf("  GetCachedElementNames called for ref='%s', fc=%d\n", ref.c_str(), fc);
         
    // Поиск по точному совпадению (уже есть)
    for (const auto& [datasetRef, cache] : datasetCache_) {
        auto it = cache.structureCache.find(ref);
        if (it != cache.structureCache.end()) {
            elementNames = it->second.elementNames;
            //printf("  ✓ Found cached element names for EXACT ref '%s'\n", ref.c_str());
            return true;
        }
    }
    
    // Если ref не содержит FC, пробуем добавить FC и поискать
    if (ref.find('[') == std::string::npos) {
        // Пробуем различные форматы FC
        std::vector<std::pair<std::string, FunctionalConstraint>> fcVariants = {
            {"ST", IEC61850_FC_ST}, {"MX", IEC61850_FC_MX}, {"CO", IEC61850_FC_CO},
            {"CF", IEC61850_FC_CF}, {"DC", IEC61850_FC_DC}, {"SP", IEC61850_FC_SP},
            {"SG", IEC61850_FC_SG}, {"BR", IEC61850_FC_BR}, {"RP", IEC61850_FC_RP},
            {"EX", IEC61850_FC_EX}, {"SR", IEC61850_FC_SR}, {"OR", IEC61850_FC_OR},
            {"BL", IEC61850_FC_BL}, {"LG", IEC61850_FC_LG}, {"GO", IEC61850_FC_GO},
            {"MS", IEC61850_FC_MS}, {"US", IEC61850_FC_US}, {"ALL", IEC61850_FC_ALL}
        };
                
        for (const auto& [fcStr, fcValue] : fcVariants) {
            if (fcValue == fc) {
                std::string refWithFc = ref + "[" + fcStr + "]";
                //printf("  Trying to find with FC: %s -> '%s'\n", fcStr.c_str(), refWithFc.c_str());
                
                for (const auto& [datasetRef, cache] : datasetCache_) {
                    auto it = cache.structureCache.find(refWithFc);
                    if (it != cache.structureCache.end()) {
                        elementNames = it->second.elementNames;
                        //printf("  ✓ Found cached element names for %s (added FC %s)\n", ref.c_str(), fcStr.c_str());
                        return true;
                    }
                }
            }
        }
    }
    
    // Если у ref есть FC, но поиск по точному совпадению не удался
    // (например, из-за различий в регистре или формате)
    size_t bracketPos = ref.find('[');
    if (bracketPos != std::string::npos) {
        std::string cleanRef = ref.substr(0, bracketPos);
        //printf("  Also trying clean ref without FC: '%s'\n", cleanRef.c_str());
        
        for (const auto& [datasetRef, cache] : datasetCache_) {
            // Ищем все ключи, которые начинаются с cleanRef
            for (const auto& [cachedRef, structInfo] : cache.structureCache) {
                if (cachedRef.find(cleanRef) == 0) {  // Начинается с cleanRef
                    elementNames = structInfo.elementNames;
                    //printf("  ✓ Found cached element names via partial match: '%s' matches '%s'\n", cleanRef.c_str(), cachedRef.c_str());
                    return true;
                }
            }
        }
    }
    
    //printf("  ✗ No cached element names found for '%s' (fc=%d)\n", ref.c_str(), fc);
    return false;
}

// Метод для кэширования имен элементов структуры
void MmsClient::CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
                                      const std::vector<std::string>& elementNames,
                                      const std::vector<MmsType>& elementTypes) {
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    //printf("  CACHE STORE: Storing structure '%s' (fc=%d) with %zu elements\n",  ref.c_str(), fc, elementNames.size());

    // Создаем запись для кэша
    StructureElementNames structInfo;
    structInfo.ref = ref;
    structInfo.fc = fc;
    structInfo.elementNames = elementNames;
    structInfo.elementTypes = elementTypes;
    
    // Ищем подходящий DataSet для кэширования
    bool cached = false;
    for (auto& [datasetRef, cache] : datasetCache_) {
        // Проверяем, содержит ли DataSet эту ссылку
        for (const auto& memberRef : cache.memberRefs) {
            std::string cleanMemberRef = memberRef;
            size_t bracketPos = memberRef.find('[');
            if (bracketPos != std::string::npos) {
                cleanMemberRef = memberRef.substr(0, bracketPos);
            }
            
            if (cleanMemberRef == ref) {
                cache.structureCache[ref] = structInfo;
                cached = true;
                //printf("Cached structure elements for %s in DataSet %s: %zu elements\n", ref.c_str(), datasetRef.c_str(), elementNames.size());
                break;
            }
        }
        if (cached) break;
    }
    
    if (!cached) {
        // Создаем новый кэш для этой структуры
        DataSetCache newCache;
        newCache.datasetRef = "dynamic_cache_" + ref;
        newCache.memberRefs.push_back(ref);
        newCache.structureCache[ref] = structInfo;
        datasetCache_[newCache.datasetRef] = newCache;
        
        //printf("Created new cache for structure %s: %zu elements\n", ref.c_str(), elementNames.size());
    }
}

static Napi::Value ProcessStructureWithCache(Napi::Env env, MmsClient* client,
                                            const std::string& fullRef, 
                                            MmsValue* structVal,
                                            int recursionDepth) {
    Napi::Object structObj = Napi::Object::New(env);
    
    if (!structVal || MmsValue_getType(structVal) != MMS_STRUCTURE) {
        return structObj;
    }
    
    // Извлекаем FC и чистую ссылку
    std::string cleanRef = fullRef;
    FunctionalConstraint fc = IEC61850_FC_ST;
    
    size_t bracketPos = fullRef.find('[');
    if (bracketPos != std::string::npos && fullRef.back() == ']') {
        std::string fcStr = fullRef.substr(bracketPos + 1, fullRef.length() - bracketPos - 2);
        cleanRef = fullRef.substr(0, bracketPos);
        fc = ParseFCFromString(fcStr);
    }
    
    int structSize = MmsValue_getArraySize(structVal);
    //printf("    Processing structure with cache %s (size=%d, fc=%d, depth=%d)\n", cleanRef.c_str(), structSize, fc, recursionDepth);
    
    // Ключевое изменение: передаем cleanRef, а не fullRef
    std::vector<std::string> elementNames;
    bool hasCachedNames = false;
    
    if (client) {
        hasCachedNames = client->GetCachedElementNames(cleanRef, fc, elementNames);
    }
    
    if (hasCachedNames && elementNames.size() == static_cast<size_t>(structSize)) {
        //printf("    SUCCESS: Using cached element names for %s (count: %zu)\n", cleanRef.c_str(), elementNames.size());
        
        for (int i = 0; i < structSize; ++i) {
            MmsValue* element = MmsValue_getElement(structVal, i);
            if (element && i < static_cast<int>(elementNames.size())) {
                const std::string& elementName = elementNames[i];
                
                // Простое и правильное построение ссылки
                std::string elementFullRef = cleanRef + "." + elementName;
                // Наследуем FC от родительской структуры
                if (bracketPos != std::string::npos) {
                    // Берем часть с FC из исходной fullRef
                    std::string fcPart = fullRef.substr(bracketPos);
                    elementFullRef += fcPart;
                }

                //printf("      Element [%d]: %s (full ref: %s)\n", i, elementName.c_str(), elementFullRef.c_str());
                
                // Рекурсивно обрабатываем элемент
                structObj.Set(elementName,
                             ProcessMmsValueWithCache(env, client, elementFullRef,
                                                     element, elementName, recursionDepth + 1));
            }
        }
    } else {
        // Используем стандартные имена IEC 61850
        printf("    Using standard IEC 61850 naming patterns (no cache)\n");
        
        std::string lowerRef = cleanRef;
        std::transform(lowerRef.begin(), lowerRef.end(), lowerRef.begin(), ::tolower);
        
        bool isST = (fc == IEC61850_FC_ST);
        bool isMX = (fc == IEC61850_FC_MX);
        // bool isDC = (fc == IEC61850_FC_DC); // Убрана неиспользуемая переменная
        
        if (isST && structSize == 3) {
            // Стандартная структура статуса: stVal, q, t
            const char* stdNames[] = {"stVal", "q", "t"};
            for (int i = 0; i < structSize; ++i) {
                MmsValue* element = MmsValue_getElement(structVal, i);
                if (element) {
                    std::string elementName = stdNames[i];
                    std::string elementFullRef = cleanRef + "." + elementName;
                    if (bracketPos != std::string::npos) {
                        elementFullRef += "[" + std::to_string(fc) + "]";
                    }
                    
                    structObj.Set(elementName,
                                 ProcessMmsValueWithCache(env, client, elementFullRef,
                                                         element, elementName, recursionDepth + 1));
                }
            }
        } else if (isMX && structSize == 3) {
            // Стандартная структура измерений: mag, q, t
            const char* stdNames[] = {"mag", "q", "t"};
            for (int i = 0; i < structSize; ++i) {
                MmsValue* element = MmsValue_getElement(structVal, i);
                if (element) {
                    std::string elementName = stdNames[i];
                    std::string elementFullRef = cleanRef + "." + elementName;
                    if (bracketPos != std::string::npos) {
                        elementFullRef += "[" + std::to_string(fc) + "]";
                    }
                    
                    structObj.Set(elementName,
                                 ProcessMmsValueWithCache(env, client, elementFullRef,
                                                         element, elementName, recursionDepth + 1));
                }
            }
        } else {
            // Неизвестная структура, используем числовые индексы
            printf("    Using numeric indices as fallback\n");
            for (int i = 0; i < structSize; ++i) {
                MmsValue* element = MmsValue_getElement(structVal, i);
                if (element) {
                    std::string indexName = std::to_string(i);
                    std::string elementFullRef = cleanRef + "." + indexName;
                    if (bracketPos != std::string::npos) {
                        elementFullRef += "[" + std::to_string(fc) + "]";
                    }
                    
                    structObj.Set(indexName,
                                 ProcessMmsValueWithCache(env, client, elementFullRef,
                                                         element, indexName, recursionDepth + 1));
                }
            }
        }
    }
    
    return structObj;
}

static Napi::Value ProcessMmsValueWithCache(Napi::Env env, MmsClient* client,
                                           const std::string& elementRef, 
                                           MmsValue* val, const std::string& elementName,
                                           int recursionDepth) {
    if (!val) {
        return env.Null();
    }
    
    const int MAX_RECURSION_DEPTH = 5;
    if (recursionDepth > MAX_RECURSION_DEPTH) {
        printf("    WARNING: Maximum recursion depth (%d) reached for %s\n", 
               MAX_RECURSION_DEPTH, elementRef.c_str());
        return env.Null();
    }
    
    int type = MmsValue_getType(val);
    
    // Если это структура, используем функцию с кэшем
    if (type == MMS_STRUCTURE) {
        //printf("    Processing structure with cache %s (depth: %d)\n", elementRef.c_str(), recursionDepth);
        return ProcessStructureWithCache(env, client, elementRef, val, recursionDepth);
    }
    
    // Для простых типов используем стандартную конвертацию
    return SafeConvertMmsValue(env, nullptr, client, elementRef, val, elementName, recursionDepth);
}

Napi::Object MmsClient::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "MmsClient", {
        InstanceMethod("connect", &MmsClient::Connect),
        InstanceMethod("readData", &MmsClient::ReadData),        
        InstanceMethod("controlObject", &MmsClient::ControlObject),
        InstanceMethod("close", &MmsClient::Close),
        InstanceMethod("getStatus", &MmsClient::GetStatus),
        InstanceMethod("getLogicalDevices", &MmsClient::GetLogicalDevices),
        InstanceMethod("readDataSetModel", &MmsClient::ReadDataSetModel),
        InstanceMethod("pollDataSetValues", &MmsClient::PollDataSetValues),
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
    isClosing_ = false;
    
    
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
    printf("\n=== MmsClient Destructor ===\n");
    //printf("  Thread ID: %zu\n", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    //printf("  clientID: %s\n", clientID_.c_str());
    
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    //printf("  Inside destructor lock:\n");
    //printf("    connected_ = %d\n", connected_);
    //printf("    running_ = %d\n", running_);
    //printf("    isClosing_ = %d\n", isClosing_);
    //printf("    connection pointer = %p\n", (void*)connection_);
    //printf("    active reports = %zu\n", activeReports_.size());
    
    // Если клиент еще не закрыт корректно, делаем это сейчас
    if (running_ || connected_) {
        printf("  WARNING: Client not properly closed before destruction!\n");
        printf("  Forcing cleanup...\n");
        
        isClosing_ = true;
        running_ = false;
        
        if (connected_ && connection_) {
            printf("  Forcing connection close on %p...\n", (void*)connection_);
            IedConnection_close(connection_);
            connected_ = false;
        }
        
        if (thread_.joinable()) {
            printf("  Thread still joinable, detaching...\n");
            thread_.detach(); // Не используем join() в деструкторе
        }
    }
    
    // Очищаем ресурсы отчетов
    printf("  Cleaning up active reports...\n");
    for (auto& [rcbRef, reportInfo] : activeReports_) {
        printf("    Report: %s\n", rcbRef.c_str());
        if (reportInfo.rcb) {
            ClientReportControlBlock_destroy(reportInfo.rcb);
            reportInfo.rcb = nullptr;
            printf("      Destroyed RCB\n");
        }
        if (reportInfo.dataSet) {
            ClientDataSet_destroy(reportInfo.dataSet);
            reportInfo.dataSet = nullptr;
            printf("      Destroyed DataSet\n");
        }
        // dataSetMembers - вектор, автоматически очистится
        // structureElementNamesCache - unordered_map, автоматически очистится
    }
    activeReports_.clear();
    printf("  All reports cleaned up\n");
    
    // Очищаем кэш
    printf("  Dataset cache entries: %zu\n", datasetCache_.size());
    datasetCache_.clear();
    printf("  Dataset cache cleared\n");
    
    // Уничтожаем соединение
    if (connection_) {
        printf("  Destroying connection at %p...\n", (void*)connection_);
        IedConnection_destroy(connection_);
        connection_ = nullptr;
        printf("  Connection destroyed\n");
    } else {
        printf("  Connection already null\n");
    }
    
    // Освобождаем TSFN
    if (tsfn_) {
        printf("  Releasing TSFN...\n");
        tsfn_.Release();
        tsfn_ = Napi::ThreadSafeFunction();
        printf("  TSFN released\n");
    } else {
        printf("  TSFN already null\n");
    }
    
    printf("=== MmsClient Destructor END ===\n\n");
}

void MmsClient::ConnectionHandler(void* parameter, IedConnection connection, IedConnectionState newState) {
    ConnectionHandlerContext* context = static_cast<ConnectionHandlerContext*>(parameter);
    MmsClient* client = context->client;
    //std::recursive_mutex* mutex = context->mutex;

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

    // Отправляем событие в JavaScript с обработкой исключений
    if (client->tsfn_) {
        client->tsfn_.NonBlockingCall([client, stateStr, isConnected, newState](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, client->clientID_.c_str()));
                eventObj.Set("type", Napi::String::New(env, "control"));
                eventObj.Set("event", Napi::String::New(env, "stateChanged"));
                eventObj.Set("state", Napi::String::New(env, stateStr));
                eventObj.Set("stateCode", Napi::Number::New(env, newState));
                eventObj.Set("isConnected", Napi::Boolean::New(env, isConnected));
                
                jsCallback.Call({Napi::String::New(env, "conn"), eventObj});
                
            } catch (const std::exception& e) {
                printf("std::exception in ConnectionHandler callback: %s\n", e.what());
            } catch (...) {
                printf("Unknown exception in ConnectionHandler callback\n");
            }
        });
    }
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
        ////std::lock_guard<std::mutex> lock(connMutex_);
        std::lock_guard<std::recursive_mutex> lock(connMutex_);
        if (running_) {
            Napi::Error::New(env, "Client already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    try {
        printf("Creating connection to %s:%d, clientID: %s\n", ip.c_str(), port, clientID_.c_str());
        running_ = true;
        isClosing_ = false;
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
                
                if (connection_) {
                    IedConnection_destroy(connection_);
                    connection_ = nullptr;
                }
                
                connection_ = IedConnection_create();

                // Установите таймауты
                //IedConnection_setConnectTimeout(connection_, 60);  // 60 секунд на подключение
                //IedConnection_setRequestTimeout(connection_, 30);  // 30 секунд на запросы
                
                IedConnection_installStateChangedHandler(connection_, ConnectionHandler, context);
                
                IedClientError error;
                IedConnection_connect(connection_, &error, currentIp.c_str(), port);

                {
                    ////std::lock_guard<std::mutex> lock(connMutex_);
                    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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
                            ////std::lock_guard<std::mutex> lock(connMutex_);
                            std::lock_guard<std::recursive_mutex> lock(connMutex_);
                            if (!connected_ || !running_) break;
                        }

                        IedConnectionState currentState = IedConnection_getState(connection_);
                        if (currentState != IED_STATE_CONNECTED) {
                            printf("Connection lost, state: %d, clientID: %s\n", currentState, clientID_.c_str());
                            
                            {
                                ////std::lock_guard<std::mutex> lock(connMutex_);
                                std::lock_guard<std::recursive_mutex> lock(connMutex_);
                                connected_ = false;
                            }
                            
                            tsfn_.NonBlockingCall([this, currentState](Napi::Env env, Napi::Function jsCallback) {
                                try {
                                    std::string stateStr;
                                    switch (currentState) {
                                        case IED_STATE_CLOSED: stateStr = "closed"; break;
                                        case IED_STATE_CLOSING: stateStr = "closing"; break;
                                        case IED_STATE_CONNECTING: stateStr = "connecting"; break;
                                        default: stateStr = "disconnected";
                                    }
                                    
                                    Napi::Object eventObj = Napi::Object::New(env);
                                    eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                                    eventObj.Set("type", Napi::String::New(env, "control"));
                                    eventObj.Set("event", Napi::String::New(env, "stateChanged"));
                                    eventObj.Set("state", Napi::String::New(env, stateStr));
                                    eventObj.Set("isConnected", Napi::Boolean::New(env, false));
                                    eventObj.Set("reason", Napi::String::New(env, "connection lost"));
                                    
                                    jsCallback.Call({Napi::String::New(env, "conn"), eventObj});
                                    
                                    printf("Connection lost event sent, state: %s, clientID: %s\n", 
                                           stateStr.c_str(), clientID_.c_str());
                                } catch (const Napi::Error& e) {
                                    printf("N-API Callback Error in connection check: %s, clientID: %s\n", 
                                           e.Message().c_str(), clientID_.c_str());
                                }
                            });
                            
                            break;
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
                                    ////std::lock_guard<std::mutex> lock(connMutex_);
                                    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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

                if (!running_) {
                    printf("Thread stopped by client, closing connection, clientID: %s\n", clientID_.c_str());
                    if (connected_) {
                        IedConnection_close(connection_);
                        {
                            ////std::lock_guard<std::mutex> lock(connMutex_);
                            std::lock_guard<std::recursive_mutex> lock(connMutex_);
                            connected_ = false;
                        }
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

Napi::Value MmsClient::Close(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    
    //printf("\n=== MmsClient::Close() called from JavaScript ===\n");
    //printf("  Thread ID: %zu (main thread? %s)\n", std::hash<std::thread::id>{}(std::this_thread::get_id()), (std::this_thread::get_id() == std::thread::id()) ? "YES" : "NO");
    //printf("  clientID: %s\n", clientID_.c_str());
    
    try {
        {
            std::lock_guard<std::recursive_mutex> lock(connMutex_);
            //printf("  Inside lock: connected_=%d, running_=%d, isClosing_=%d\n", connected_, running_, isClosing_);
            //printf("  Connection pointer: %p\n", (void*)connection_);
            
            // Проверяем, не закрываем ли уже
            if (isClosing_) {
                //printf("  WARNING: Already closing, ignoring duplicate call\n");
                deferred.Resolve(Napi::Boolean::New(env, false));
                return deferred.Promise();
            }
            
            isClosing_ = true;
            //printf("  Set isClosing_=true\n");
            
            if (running_) {
                running_ = false;
                //printf("  Set running_=false\n");
                
                if (connected_) {
                    //printf("  *** CLOSING ACTIVE CONNECTION ***\n");
                    //printf("  Calling IedConnection_close() on %p...\n", (void*)connection_);
                    
                    // Сохраняем состояние до закрытия
                    IedConnectionState beforeClose = IED_STATE_CLOSED;
                    if (connection_) {
                        beforeClose = IedConnection_getState(connection_);
                    }
                    
                    // Сначала отправляем событие в JS
                    tsfn_.NonBlockingCall([this, beforeClose](Napi::Env env, Napi::Function jsCallback) {
                        try {
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "stateChanged"));
                            eventObj.Set("state", Napi::String::New(env, "closing"));
                            eventObj.Set("isConnected", Napi::Boolean::New(env, false));
                            eventObj.Set("reason", Napi::String::New(env, std::string("client initiated close, previous state: ") + std::to_string(beforeClose)));
                            jsCallback.Call({Napi::String::New(env, "conn"), eventObj});
                            printf("  'closing' event sent to JS (previous state: %d)\n", beforeClose);
                        } catch (const Napi::Error& e) {
                            printf("  N-API Callback Error in Close: %s\n", e.Message().c_str());
                        }
                    });
                    
                    // Затем закрываем соединение
                    IedConnection_close(connection_);
                    //printf("  IedConnection_close() completed\n");
                    
                    connected_ = false;
                    //printf("  Set connected_=false\n");
                    
                    // Проверяем состояние после закрытия
                    if (connection_) {
                        IedConnectionState afterClose = IedConnection_getState(connection_);
                        //printf("  State after close: %d\n", afterClose);
                    }
                } else {
                    //printf("  Already disconnected, skipping IedConnection_close()\n");
                }
            } else {
                //printf("  Client not running, no connection to close\n");
            }
        }
        
        //printf("  Waiting for connection thread to finish...\n");
        if (thread_.joinable()) {
            //printf("  Thread joinable, calling join()...\n");
            thread_.join();
            //printf("  Thread joined successfully\n");
        } else {
            //printf("  Thread not joinable\n");
        }
        
        {
            std::lock_guard<std::recursive_mutex> lock(connMutex_);
            //printf("  Cleaning up resources...\n");
            
            // Освобождаем ресурсы отчетов
            //printf("  Active reports to clean up: %zu\n", activeReports_.size());
            for (auto& [rcbRef, reportInfo] : activeReports_) {
                //printf("    Cleaning up report: %s\n", rcbRef.c_str());
                if (reportInfo.rcb) {
                    ClientReportControlBlock_destroy(reportInfo.rcb);
                    reportInfo.rcb = nullptr;
                    //printf("      Destroyed RCB\n");
                }
                if (reportInfo.dataSet) {
                    ClientDataSet_destroy(reportInfo.dataSet);
                    reportInfo.dataSet = nullptr;
                    //printf("      Destroyed DataSet\n");
                }
                // dataSetMembers и structureElementNamesCache очистятся автоматически
            }
            activeReports_.clear();
            //printf("  All reports cleaned up\n");
            
            // Очищаем кэш
            //printf("  Dataset cache entries: %zu\n", datasetCache_.size());
            datasetCache_.clear();
            //printf("  Dataset cache cleared\n");
            
            // Уничтожаем соединение
            if (connection_) {
                //printf("  Destroying connection at %p...\n", (void*)connection_);
                IedConnection_destroy(connection_);
                connection_ = nullptr;
                //printf("  Connection destroyed\n");
            } else {
                //printf("  Connection already null\n");
            }
            
            // Освобождаем TSFN
            if (tsfn_) {
                //printf("  Releasing TSFN...\n");
                tsfn_.Release();
                tsfn_ = Napi::ThreadSafeFunction();
                //printf("  TSFN released\n");
            } else {
                //printf("  TSFN already null\n");
            }
        }
        
        //printf("=== MmsClient::Close() completed successfully ===\n\n");
        deferred.Resolve(Napi::Boolean::New(env, true));
    } catch (const std::exception& e) {
        printf("  EXCEPTION in Close: %s, clientID: %s\n", e.what(), clientID_.c_str());
        deferred.Reject(Napi::Error::New(env, std::string("Close failed: ") + e.what()).Value());
    }
    
    return deferred.Promise();
}

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

    if (data.type < 0 || data.type > 14) {
        data.isValid = false;
        data.errorReason = "Unsupported MMS type";
        return data;
    }

    switch (data.type) {
        case MMS_STRUCTURE: {
            int size = MmsValue_getArraySize(val);
            
            printf("ConvertMmsValueToResultData: Processing structure attrName='%s', size=%d\n", 
                   attrName.c_str(), size);
            
            // Для структур просто собираем элементы с числовыми индексами
            // Реальные имена будут получены в ReadDataSetValues через getVariableSpecification
            for (int i = 0; i < size; ++i) {
                MmsValue* el = MmsValue_getElement(val, i);
                if (el) {
                    std::string elementName = std::to_string(i); // Временное имя
                    MmsClient::ResultData rd = ConvertMmsValueToResultData(el, elementName);
                    data.structureElements.push_back(rd);
                }
            }
            break;
        }
        
        // Обработка других типов остается без изменений
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
                    default: data.stringValue = "unknown(" + std::to_string(data.intValue) + ")"; break;
                }
            }
            break;

        case MMS_BOOLEAN:
            data.boolValue = MmsValue_getBoolean(val);
            break;

        case MMS_VISIBLE_STRING: {
            const char* str = MmsValue_toString(val);
            data.stringValue = str ? str : "";
            break;
        }

        case MMS_UTC_TIME: {
            data.intValue = static_cast<int64_t>(MmsValue_getUtcTimeInMs(val));
            break;
        }

        case MMS_BIT_STRING: {
            uint32_t bits = MmsValue_getBitStringAsInteger(val);
            data.intValue = static_cast<int64_t>(bits);
            
            // ТОЛЬКО для stVal оставляем преобразование в строку
            // Для q больше НЕ преобразуем в строку флагов
            int size = MmsValue_getBitStringSize(val);
            
            if (size == 2 && attrName.find("stVal") != std::string::npos) {
                uint32_t msbValue = 0;
                uint32_t lsbValue = bits;
                
                for (int i = 0; i < 2; i++) {
                    int bit = (lsbValue >> i) & 1;
                    msbValue |= (bit << (1 - i));
                }
                
                data.intValue = static_cast<int64_t>(msbValue);
                
                switch (msbValue) {
                    case 0: data.stringValue = "intermediate-state"; break;
                    case 1: data.stringValue = "off"; break;
                    case 2: data.stringValue = "on"; break;
                    case 3: data.stringValue = "bad-state"; break;
                    default: data.stringValue = "unknown(" + std::to_string(msbValue) + ")"; break;
                }
            }
            // Для q НЕ формируем строку флагов - просто оставляем битовую строку
            break;
        }

        case MMS_ARRAY: {
            int size = MmsValue_getArraySize(val);
            for (int i = 0; i < size; ++i) {
                MmsValue* el = MmsValue_getElement(val, i);
                if (el) {
                    data.arrayElements.push_back(ConvertMmsValueToResultData(el, attrName));
                }
            }
            break;
        }

        default:
            data.isValid = false;
            data.errorReason = "Unsupported MMS type";
            break;
    }
    
    return data;
}

static Napi::Value ResultDataToNapi(Napi::Env env, const MmsClient::ResultData& data, const std::string& attrName = "") {
    try {
        if (!data.isValid) {
            return Napi::String::New(env, data.errorReason);
        }
        
        switch (data.type) {
            case MMS_FLOAT:
                return Napi::Number::New(env, data.floatValue);
            case MMS_INTEGER:
            case MMS_UNSIGNED:
                if (!data.stringValue.empty()) {
                    return Napi::String::New(env, data.stringValue);
                }
                if (attrName.find("OpCap") != std::string::npos || attrName.find("CBOpCap") != std::string::npos) {
                    return Napi::Number::New(env, static_cast<double>(data.intValue));
                }
                return Napi::Number::New(env, static_cast<double>(data.intValue));
            case MMS_BOOLEAN:
                return Napi::Boolean::New(env, data.boolValue);
            case MMS_VISIBLE_STRING:
                return Napi::String::New(env, data.stringValue);
            case MMS_UTC_TIME:
                return Napi::Number::New(env, static_cast<double>(data.intValue));
            case MMS_BIT_STRING:
                if (!data.stringValue.empty()) {
                    return Napi::String::New(env, data.stringValue);
                }
                return Napi::Number::New(env, static_cast<double>(data.intValue));
            case MMS_OCTET_STRING:
                return Napi::String::New(env, data.stringValue);
            case MMS_STRUCTURE: {
                //Napi::Object obj = Napi::Object::New(env);
                
                std::string name = attrName;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                
                bool isST = false, isMX = false, isDC = false;
                
                if (name.size() >= 4) {
                    std::string ending = name.substr(name.size() - 4);
                    isST = (ending == "[st]");
                    isMX = (ending == "[mx]");
                    isDC = (ending == "[dc]");
                }
                
                bool hasSumSwARs = (name.find("sumswars") != std::string::npos);
                bool hasEEName = (name.find("eename") != std::string::npos);
                bool hasAnIn = (name.find("anin") != std::string::npos);
                
                if (hasSumSwARs && isST && data.structureElements.size() >= 3) {
                    Napi::Object stObj = Napi::Object::New(env);
                    if (data.structureElements.size() > 0) 
                        stObj.Set("actVal", ResultDataToNapi(env, data.structureElements[0], "actVal"));
                    if (data.structureElements.size() > 1) 
                        stObj.Set("q", ResultDataToNapi(env, data.structureElements[1], "q"));
                    if (data.structureElements.size() > 2) 
                        stObj.Set("t", ResultDataToNapi(env, data.structureElements[2], "t"));
                    return stObj;
                }
                else if (name.find("phynam") != std::string::npos && isDC) {
                    Napi::Object dplObj = Napi::Object::New(env);
                    
                    std::vector<std::string> dplFieldNames = {"vendor", "hwRev", "swRev", 
                                                              "serialNum", "d", "configRev"};
                    
                    for (size_t i = 0; i < data.structureElements.size() && i < dplFieldNames.size(); ++i) {
                        dplObj.Set(dplFieldNames[i], 
                                  ResultDataToNapi(env, data.structureElements[i], dplFieldNames[i]));
                    }
                    return dplObj;
                }
                else if (hasEEName && isDC && !data.structureElements.empty()) {
                    Napi::Object dcObj = Napi::Object::New(env);
                    dcObj.Set("vendor", ResultDataToNapi(env, data.structureElements[0]));
                    return dcObj;
                }
                else if (hasAnIn && isMX && data.structureElements.size() >= 3) {
                    Napi::Object mxObj = Napi::Object::New(env);
                    
                    if (data.structureElements[0].type == MMS_STRUCTURE && 
                        !data.structureElements[0].structureElements.empty()) {
                        Napi::Object magObj = Napi::Object::New(env);
                        magObj.Set("f", ResultDataToNapi(env, data.structureElements[0].structureElements[0]));
                        mxObj.Set("mag", magObj);
                    } else {
                        mxObj.Set("mag", ResultDataToNapi(env, data.structureElements[0]));
                    }
                    
                    mxObj.Set("q", ResultDataToNapi(env, data.structureElements[1], "q"));
                    mxObj.Set("t", ResultDataToNapi(env, data.structureElements[2], "t"));
                    return mxObj;
                }
                else if (name.find("fltdiskm") != std::string::npos && isMX && data.structureElements.size() == 3) {
                    Napi::Object mxObj = Napi::Object::New(env);
                    
                    if (data.structureElements[0].type == MMS_STRUCTURE) {
                        mxObj.Set("mag", ResultDataToNapi(env, data.structureElements[0], "mag"));
                    } else {
                        mxObj.Set("mag", ResultDataToNapi(env, data.structureElements[0]));
                    }
                    
                    mxObj.Set("q", ResultDataToNapi(env, data.structureElements[1], "q"));
                    mxObj.Set("t", ResultDataToNapi(env, data.structureElements[2], "t"));
                    return mxObj;
                }
                else if (isST && data.structureElements.size() >= 3) {
                    Napi::Object stObj = Napi::Object::New(env);
                    stObj.Set("stVal", ResultDataToNapi(env, data.structureElements[0], "stVal"));
                    stObj.Set("q", ResultDataToNapi(env, data.structureElements[1], "q"));
                    stObj.Set("t", ResultDataToNapi(env, data.structureElements[2], "t"));
                    return stObj;
                }
                else {
                    Napi::Object defaultObj = Napi::Object::New(env);
                    for (size_t i = 0; i < data.structureElements.size(); ++i) {
                        defaultObj.Set(std::to_string(i), ResultDataToNapi(env, data.structureElements[i]));
                    }
                    return defaultObj;
                }
            }
            
            case MMS_ARRAY: {
                Napi::Array arr = Napi::Array::New(env, data.arrayElements.size());
                for (size_t i = 0; i < data.arrayElements.size(); ++i) {
                    arr.Set(i, ResultDataToNapi(env, data.arrayElements[i]));
                }
                return arr;
            }
            
            default:
                return Napi::String::New(env, "type_" + std::to_string(data.type));
        }
    } catch (const std::exception& e) {
        printf("ResultDataToNapi std::exception: %s\n", e.what());
        return Napi::String::New(env, "Conversion Error");
    } catch (...) {
        printf("ResultDataToNapi unknown exception\n");
        return Napi::String::New(env, "Unknown Error");
    }
}

static std::string ResultDataToString(const MmsClient::ResultData& data) {
    if (!data.isValid) return data.errorReason;
    switch (data.type) {
        case MMS_FLOAT: return std::to_string(data.floatValue);
        case MMS_INTEGER:
        case MMS_UNSIGNED: return data.stringValue.empty() ? std::to_string(data.intValue) : data.stringValue;
        case MMS_BOOLEAN: return data.boolValue ? "true" : "false";
        case MMS_VISIBLE_STRING: return data.stringValue;
        case MMS_UTC_TIME: return std::to_string(data.intValue);
        case MMS_OCTET_STRING: return data.stringValue;
        case MMS_BIT_STRING: return data.stringValue.empty() ? std::to_string(data.intValue) : data.stringValue;
        case MMS_STRUCTURE:
        case MMS_ARRAY: return "complex";
        default: return "type_" + std::to_string(data.type);
    }
}

Napi::Value MmsValueToNapi(Napi::Env env, MmsValue* value) {
    if (!value) return env.Null();
    MmsClient::ResultData data = ConvertMmsValueToResultData(value, "");
    return ResultDataToNapi(env, data);
}

// Единая функция для преобразования MMS значения в NAPI значение
static Napi::Value SafeConvertMmsValue(Napi::Env env, IedConnection connection, MmsClient* client,
                                      const std::string& elementRef, 
                                      MmsValue* val, const std::string& elementName,
                                      int recursionDepth) {
    if (!val) {
        return env.Null();
    }
    
    const int MAX_RECURSION_DEPTH = 5;
    if (recursionDepth > MAX_RECURSION_DEPTH) {
        printf("    WARNING: Maximum recursion depth (%d) reached for %s\n", 
               MAX_RECURSION_DEPTH, elementRef.c_str());
        return env.Null();
    }
    
    int type = MmsValue_getType(val);
    
    // Если это структура, используем улучшенную функцию с кэшем
    if (type == MMS_STRUCTURE) {
        printf("    Processing structure %s with cache (depth: %d)\n", elementRef.c_str(), recursionDepth);
        return ProcessStructureWithCache(env, client, elementRef, val, recursionDepth);
    }
    
  // Для простых типов используем стандартную конвертацию
    switch (type) {
        case MMS_FLOAT:
            return Napi::Number::New(env, MmsValue_toFloat(val));
            
        case MMS_INTEGER:
        case MMS_UNSIGNED:
            return Napi::Number::New(env, MmsValue_toInt64(val));
            
        case MMS_BOOLEAN:
            return Napi::Boolean::New(env, MmsValue_getBoolean(val));
            
        case MMS_VISIBLE_STRING: {
            const char* str = MmsValue_toString(val);
            return Napi::String::New(env, str ? str : "");
        }
            
        case MMS_UTC_TIME:
            return Napi::Number::New(env, static_cast<double>(MmsValue_getUtcTimeInMs(val)));
            
        case MMS_BIT_STRING: {
            uint32_t bits = MmsValue_getBitStringAsInteger(val);
            
            // Для DPC (stVal)
            if (elementName.find("stVal") != std::string::npos && MmsValue_getBitStringSize(val) == 2) {
                // Для DPC (stVal) - 2-битное значение
                // В стандарте IEC 61850 для DPC:
                // 00 (0) = intermediate-state
                // 01 (1) = off
                // 10 (2) = on
                // 11 (3) = bad-state
                uint32_t msbValue = 0;
                uint32_t lsbValue = bits;
                
                // Преобразуем LSB-first в MSB-first
                for (int i = 0; i < 2; i++) {
                    int bit = (lsbValue >> i) & 1;
                    msbValue |= (bit << (1 - i));
                }
                
                switch (msbValue) {
                    case 0: return Napi::String::New(env, "intermediate-state");
                    case 1: return Napi::String::New(env, "off");
                    case 2: return Napi::String::New(env, "on");
                    case 3: return Napi::String::New(env, "bad-state");
                    default: return Napi::Number::New(env, static_cast<double>(msbValue));
                }
            }
            
            // Для качества (q) и других битовых строк возвращаем число (битовую строку)
            return Napi::Number::New(env, static_cast<double>(bits));
        }
            
        case MMS_ARRAY: {
            int size = MmsValue_getArraySize(val);
            Napi::Array arr = Napi::Array::New(env, size);
            
            for (int i = 0; i < size; ++i) {
                MmsValue* element = MmsValue_getElement(val, i);
                if (element) {
                    // Для элементов массива создаем ссылку с индексом
                    std::string arrayElementRef = elementRef + "[" + std::to_string(i) + "]";
                    arr.Set(i, SafeConvertMmsValue(env, connection, client, arrayElementRef, 
                               element, elementName, recursionDepth));
                }
            }
            return arr;
        }
            
        default:
            return Napi::String::New(env, "type_" + std::to_string(type));
    }
}

// Вспомогательная функция для применения кэша имён
static void EnhanceResultDataWithCachedNames(MmsClient* client,
                                             MmsClient::ResultData& data,
                                             const std::string& fullRef,
                                             int depth = 0) {
    const int MAX_DEPTH = 5;
    if (depth > MAX_DEPTH || data.type != MMS_STRUCTURE) return;

    std::string cleanRef = fullRef;
    FunctionalConstraint fc = IEC61850_FC_ST;

    size_t bracketPos = fullRef.find('[');
    if (bracketPos != std::string::npos && fullRef.back() == ']') {
        std::string fcStr = fullRef.substr(bracketPos + 1, fullRef.length() - bracketPos - 2);
        cleanRef = fullRef.substr(0, bracketPos);
        fc = ParseFCFromString(fcStr);
    }

    std::vector<std::string> elementNames;
    if (client->GetCachedElementNames(cleanRef, fc, elementNames) &&
        elementNames.size() == data.structureElements.size()) {
        data.structureElementNames = elementNames;

        for (size_t i = 0; i < data.structureElements.size(); ++i) {
            std::string childRef = cleanRef + "." + elementNames[i];
            if (bracketPos != std::string::npos) {
                childRef += fullRef.substr(bracketPos);
            }
            EnhanceResultDataWithCachedNames(client, data.structureElements[i], childRef, depth + 1);
        }
    }
}

Napi::Value MmsClient::ReadDataSetModel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // Проверка подключения
    if (!connected_) {
        Napi::Error::New(env, "Client not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    // Получаем список DataSet ссылок
    std::vector<std::string> datasetRefs;

    if (info.Length() != 1 || (!info[0].IsString() && !info[0].IsArray())) {
        Napi::TypeError::New(env, "Expected string or array of strings").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info[0].IsString()) {
        datasetRefs.push_back(info[0].As<Napi::String>().Utf8Value());
    } else {
        Napi::Array arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            if (arr.Get(i).IsString()) {
                datasetRefs.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
            }
        }
    }

    if (datasetRefs.empty()) {
        Napi::TypeError::New(env, "No valid dataset references provided").ThrowAsJavaScriptException();
        return env.Null();
    }

    printf("ReadDataSetModel: Reading %zu datasets\n", datasetRefs.size());
    for (const auto& ref : datasetRefs) {
        printf("  - %s\n", ref.c_str());
    }

    // Создаём Promise для асинхронной операции
    auto deferred = Napi::Promise::Deferred::New(env);

    // Создаём и запускаем асинхронный воркер
    // Передаём все необходимые ресурсы, а не доступ к приватным полям
    ReadDataSetModelWorker* worker = new ReadDataSetModelWorker(
        this,           // client
        connection_,    // IedConnection - передаём напрямую
        connMutex_,     // mutex - передаём по ссылке
        env,            // Napi::Env для создания объектов в OnOK
        datasetRefs,    // список датасетов
        deferred        // Promise::Deferred для разрешения/отклонения
    );

    worker->Queue();
    return deferred.Promise();
}

// Асинхронная функция чтения значений DataSet методом поллинга
Napi::Value MmsClient::PollDataSetValues(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!connected_) {
        Napi::Error::New(env, "Client not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::vector<std::string> datasetRefs;

    if (info.Length() != 1 || (!info[0].IsString() && !info[0].IsArray())) {
        Napi::TypeError::New(env, "Expected string or array of strings").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info[0].IsString()) {
        datasetRefs.push_back(info[0].As<Napi::String>().Utf8Value());
    } else {
        Napi::Array arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            if (arr.Get(i).IsString()) {
                datasetRefs.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
            }
        }
    }

    if (datasetRefs.empty()) {
        Napi::TypeError::New(env, "No valid dataset references provided").ThrowAsJavaScriptException();
        return env.Null();
    }

    // Создаём Promise
    auto deferred = Napi::Promise::Deferred::New(env);

    // Создаём и запускаем асинхронный воркер
    PollDataSetValuesWorker* worker = new PollDataSetValuesWorker(
        this,
        connection_,
        connMutex_,
        env,
        datasetRefs,
        deferred
    );

    worker->Queue();
    return deferred.Promise();
}

// Функция для получения корневых узлов
Napi::Value MmsClient::GetRootNodes(Napi::Env env) {
    IedClientError error;
    
    // Получаем список Logical Devices
    LinkedList deviceList = IedConnection_getLogicalDeviceList(connection_, &error);
    if (error != IED_ERROR_OK || !deviceList) {
        printf("GetRootNodes: Failed to get logical device list, error: %d\n", error);
        return Napi::Array::New(env, 0);
    }
    
    Napi::Array resultArray = Napi::Array::New(env);
    uint32_t deviceIndex = 0;
    
    LinkedList device = LinkedList_getNext(deviceList);
    while (device) {
        char* ldName = (char*)device->data;
        if (!ldName) { 
            device = LinkedList_getNext(device); 
            continue; 
        }
        
        printf("Processing logical device: %s\n", ldName);
        
        // Получаем список Logical Nodes для устройства
        LinkedList logicalNodes = IedConnection_getLogicalDeviceDirectory(connection_, &error, ldName);
        if (error == IED_ERROR_OK && logicalNodes) {
            LinkedList ln = LinkedList_getNext(logicalNodes);
            while (ln) {
                char* lnName = (char*)ln->data;
                if (!lnName) { 
                    ln = LinkedList_getNext(ln); 
                    continue; 
                }
                
                std::string lnRef = std::string(ldName) + "/" + lnName;
                printf("  Found logical node: %s\n", lnRef.c_str());
                
                Napi::Object lnObj = Napi::Object::New(env);
                lnObj.Set("name", Napi::String::New(env, lnName));
                lnObj.Set("reference", Napi::String::New(env, lnRef));
                
                // Получаем DataSets для этого LN
                Napi::Array dsArray = Napi::Array::New(env);
                uint32_t dsIndex = 0;
                
                LinkedList dataSets = IedConnection_getLogicalNodeDirectory(
                    connection_, &error, lnRef.c_str(), ACSI_CLASS_DATA_SET);
                
                if (error == IED_ERROR_OK && dataSets) {
                    LinkedList ds = LinkedList_getNext(dataSets);
                    while (ds) {
                        char* dsName = (char*)ds->data;
                        if (!dsName) { 
                            ds = LinkedList_getNext(ds); 
                            continue; 
                        }
                        
                        std::string dsRef = lnRef + "." + dsName;
                        Napi::Object dsObj = Napi::Object::New(env);
                        dsObj.Set("name", Napi::String::New(env, dsName));
                        dsObj.Set("reference", Napi::String::New(env, dsRef));
                        dsObj.Set("type", Napi::String::New(env, "dataset"));
                        
                        dsArray.Set(dsIndex++, dsObj);
                        ds = LinkedList_getNext(ds);
                    }
                    LinkedList_destroy(dataSets);
                }
                
                lnObj.Set("dataSets", dsArray);
                
                // Получаем Reports для этого LN
                Napi::Array reportsArray = Napi::Array::New(env);
                uint32_t reportIndex = 0;
                
                // Используем getLogicalNodeVariables для получения всех объектов
                LinkedList dataObjects = IedConnection_getLogicalNodeVariables(connection_, &error, lnRef.c_str());
                
                if (error == IED_ERROR_OK && dataObjects) {
                    LinkedList dObj = LinkedList_getNext(dataObjects);
                    while (dObj) {
                        char* doName = (char*)dObj->data;
                        if (!doName) { 
                            dObj = LinkedList_getNext(dObj); 
                            continue; 
                        }
                        
                        std::string doNameStr(doName);
                        
                        // Проверяем, является ли этот объект отчетом
                        bool isReport = false;
                        std::string reportType = "";
                        
                        if (doNameStr.find("RP$") == 0 || doNameStr.find("BR$") == 0) {
                            // Считаем количество знаков $
                            size_t dollarCount = 0;
                            for (char c : doNameStr) {
                                if (c == '$') dollarCount++;
                            }
                            
                            // Если ровно один $, то это основной объект отчета
                            if (dollarCount == 1) {
                                isReport = true;
                                if (doNameStr.find("RP$") == 0) {
                                    reportType = "RP";
                                } else if (doNameStr.find("BR$") == 0) {
                                    reportType = "BR";
                                }
                            }
                        }
                        
                        if (isReport) {
                            std::string doRef = lnRef + "." + doName;
                            
                            Napi::Object reportObj = Napi::Object::New(env);
                            reportObj.Set("name", Napi::String::New(env, doName));
                            reportObj.Set("reference", Napi::String::New(env, doRef));
                            reportObj.Set("type", Napi::String::New(env, reportType));
                            reportObj.Set("description", Napi::String::New(env, 
                                (reportType == "RP") ? "Unbuffered Report" : "Buffered Report"));
                            
                            reportsArray.Set(reportIndex++, reportObj);
                        }
                        
                        dObj = LinkedList_getNext(dObj);
                    }
                    LinkedList_destroy(dataObjects);
                }
                
                lnObj.Set("reports", reportsArray);
                resultArray.Set(deviceIndex++, lnObj);
                ln = LinkedList_getNext(ln);
            }
            LinkedList_destroy(logicalNodes);
        }
        
        device = LinkedList_getNext(device);
    }
    LinkedList_destroy(deviceList);
    
    return resultArray;
}

// Функция для обхода конкретного объекта
Napi::Value MmsClient::BrowseSpecificObject(Napi::Env env, const std::string& ref) {
    printf("BrowseSpecificObject: %s\n", ref.c_str());
    
    // Проверяем, что это за тип объекта
    // 1. LN: формат "LD/LN" (например, "WAGO61850ServerDevice/LLN0")
    // 2. DO: формат "LD/LN.DO" (например, "WAGO61850ServerDevice/LLN0.Beh")
    // 3. DS: формат "LD/LN.DS" (например, "WAGO61850ServerDevice/LLN0.DataSet01")
    // 4. R: формат "LD/LN.R" (например, "WAGO61850ServerDevice/LLN0.RP$ReportBlock0101")
    
    // Проверяем, содержит ли ссылка точку
    size_t dotPos = ref.find('.');
    
    if (dotPos == std::string::npos) {
        // Нет точки - это должен быть LN
        return GetLogicalNodeDetails(env, ref);
    } else {
        // Есть точка - это может быть DO, DS или R
        // Извлекаем часть до точки
        std::string basePart = ref.substr(0, dotPos);
        std::string objectPart = ref.substr(dotPos + 1);
        
        // Проверяем, является ли это DataSet
        IedClientError error;
        bool isDeletable = false;
        
        LinkedList dataSetMembers = IedConnection_getDataSetDirectory(
            connection_, &error, ref.c_str(), &isDeletable);
        
        if (error == IED_ERROR_OK && dataSetMembers) {
            // Это DataSet
            LinkedList_destroy(dataSetMembers);
            return GetDataSetDetails(env, ref);
        }
        
        // Проверяем, является ли это отчетом (содержит $)
        if (objectPart.find('$') != std::string::npos) {
            // Содержит $ - вероятно, это отчет
            return GetReportDetails(env, ref);
        } else {
            // Скорее всего, это DataObject
            return GetDataObjectDetails(env, ref);
        }
    }
}

// Функция для получения деталей Logical Node
Napi::Value MmsClient::GetLogicalNodeDetails(Napi::Env env, const std::string& lnRef) {
    printf("GetLogicalNodeDetails: %s\n", lnRef.c_str());
    
    IedClientError error;
    Napi::Object result = Napi::Object::New(env);
    result.Set("type", Napi::String::New(env, "logicalNode"));
    result.Set("reference", Napi::String::New(env, lnRef));
    
    // Извлекаем имя LN из ссылки
    size_t slashPos = lnRef.find_last_of('/');
    if (slashPos != std::string::npos) {
        std::string lnName = lnRef.substr(slashPos + 1);
        result.Set("name", Napi::String::New(env, lnName));
    }
    
    // ВАЖНОЕ ИСПРАВЛЕНИЕ: Используем getLogicalNodeDirectory с классом DATA_OBJECT
    // чтобы получить только DataObjects, а не все переменные
    Napi::Array doArray = Napi::Array::New(env);
    uint32_t doIndex = 0;
    
    LinkedList dataObjects = IedConnection_getLogicalNodeDirectory(
        connection_, &error, lnRef.c_str(), ACSI_CLASS_DATA_OBJECT);
    
    if (error == IED_ERROR_OK && dataObjects) {
        printf("  Found DataObjects for %s:\n", lnRef.c_str());
        LinkedList dObj = LinkedList_getNext(dataObjects);
        while (dObj) {
            char* doName = (char*)dObj->data;
            if (!doName) { 
                dObj = LinkedList_getNext(dObj); 
                continue; 
            }
            
            std::string doRef = lnRef + "." + doName;
            printf("    DataObject: %s\n", doName);
            
            // Проверяем, не является ли это отчетом (пропускаем отчеты)
            std::string doNameStr(doName);
            if (doNameStr.find("RP$") == 0 || doNameStr.find("BR$") == 0) {
                // Это отчет, пропускаем - отчеты обрабатываем отдельно
                dObj = LinkedList_getNext(dObj);
                continue;
            }
            
            // Это обычный DataObject
            Napi::Object doObj = Napi::Object::New(env);
            doObj.Set("name", Napi::String::New(env, doName));
            doObj.Set("reference", Napi::String::New(env, doRef));
            doObj.Set("type", Napi::String::New(env, "dataObject"));
            
            // Определяем тип CDC по имени
            std::string doStr(doName);
            std::transform(doStr.begin(), doStr.end(), doStr.begin(), ::tolower);
            
            if (doStr.find("pos") != std::string::npos ||
                doStr.find("swi") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "DPC"));
            } else if (doStr.find("ind") != std::string::npos ||
                       doStr.find("alm") != std::string::npos ||
                       doStr.find("tr") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "SPS"));
            } else if (doStr.find("anin") != std::string::npos ||
                       doStr.find("mag") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "MV"));
            } else if (doStr.find("mod") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "INC"));
            } else if (doStr.find("beh") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "INS"));
            } else if (doStr.find("max") != std::string::npos ||
                       doStr.find("set") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "ASG"));
            } else if (doStr.find("phy") != std::string::npos ||
                       doStr.find("nam") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "LPL"));
            } else if (doStr.find("str") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "DPL"));
            } else if (doStr.find("ledrs") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "INS"));
            } else if (doStr.find("health") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "INS"));
            } else if (doStr.find("loc") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "SPS"));
            } else if (doStr.find("optmh") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "INS"));
            } else if (doStr.find("diag") != std::string::npos) {
                doObj.Set("cdc", Napi::String::New(env, "INS"));
            } else {
                doObj.Set("cdc", Napi::String::New(env, "Unknown"));
            }
            
            doArray.Set(doIndex++, doObj);
            dObj = LinkedList_getNext(dObj);
        }
        LinkedList_destroy(dataObjects);
    } else {
        printf("  ERROR: Failed to get DataObjects for %s, error: %d\n", lnRef.c_str(), error);
    }
    
    result.Set("dataObjects", doArray);
    result.Set("dataObjectsCount", Napi::Number::New(env, doIndex));
    
    // Также получаем DataSets для этого LN
    Napi::Array dsArray = Napi::Array::New(env);
    uint32_t dsIndex = 0;
    
    LinkedList dataSets = IedConnection_getLogicalNodeDirectory(
        connection_, &error, lnRef.c_str(), ACSI_CLASS_DATA_SET);
    
    if (error == IED_ERROR_OK && dataSets) {
        printf("  Found DataSets for %s:\n", lnRef.c_str());
        LinkedList ds = LinkedList_getNext(dataSets);
        while (ds) {
            char* dsName = (char*)ds->data;
            if (!dsName) { 
                ds = LinkedList_getNext(ds); 
                continue; 
            }
            
            std::string dsRef = lnRef + "." + dsName;
            printf("    DataSet: %s\n", dsName);
            
            Napi::Object dsObj = Napi::Object::New(env);
            dsObj.Set("name", Napi::String::New(env, dsName));
            dsObj.Set("reference", Napi::String::New(env, dsRef));
            dsObj.Set("type", Napi::String::New(env, "dataset"));
            
            dsArray.Set(dsIndex++, dsObj);
            ds = LinkedList_getNext(ds);
        }
        LinkedList_destroy(dataSets);
    }
    
    result.Set("dataSets", dsArray);
    result.Set("dataSetsCount", Napi::Number::New(env, dsIndex));
    
    return result;
}

// Функция для получения деталей DataObject
Napi::Value MmsClient::GetDataObjectDetails(Napi::Env env, const std::string& doRef) {
    printf("GetDataObjectDetails: %s\n", doRef.c_str());
    
    IedClientError error;
    Napi::Object result = Napi::Object::New(env);
    result.Set("type", Napi::String::New(env, "dataObject"));
    result.Set("reference", Napi::String::New(env, doRef));
    
    // Извлекаем имя DO из ссылки
    size_t dotPos = doRef.find_last_of('.');
    if (dotPos != std::string::npos) {
        std::string doName = doRef.substr(dotPos + 1);
        result.Set("name", Napi::String::New(env, doName));
    }
    
    // Рекурсивная функция для получения атрибутов
    std::function<Napi::Value(const std::string&)> getAttributes;
    getAttributes = [&](const std::string& ref) -> Napi::Value {
        Napi::Object attributes = Napi::Object::New(env);
        LinkedList attrList = IedConnection_getDataDirectory(connection_, &error, ref.c_str());
        
        if (error == IED_ERROR_OK && attrList) {
            LinkedList entry = attrList;
            while (entry) {
                if (entry->data) {
                    char* attrName = (char*)entry->data;
                    std::string attrRef = ref + "." + attrName;
                    
                    // Определяем FC
                    FunctionalConstraint fc = IEC61850_FC_ST;
                    std::string attrNameStr(attrName);
                    
                    if (attrNameStr.find("stVal") != std::string::npos ||
                        attrNameStr.find("q") != std::string::npos ||
                        attrNameStr.find("t") != std::string::npos) {
                        fc = IEC61850_FC_ST;
                    } else if (attrNameStr.find("mag") != std::string::npos ||
                               attrNameStr.find("range") != std::string::npos) {
                        fc = IEC61850_FC_MX;
                    } else if (attrNameStr.find("ctlModel") != std::string::npos ||
                               attrNameStr.find("Oper") != std::string::npos ||
                               attrNameStr.find("Cancel") != std::string::npos) {
                        fc = IEC61850_FC_CF;
                    } else if (attrNameStr.find("NamPlt") != std::string::npos ||
                               attrNameStr.find("PhyNam") != std::string::npos ||
                               attrNameStr.find("vendor") != std::string::npos) {
                        fc = IEC61850_FC_DC;
                    } else if (attrNameStr.find("AnIn") != std::string::npos) {
                        fc = IEC61850_FC_MX;
                    }
                    
                    // Проверяем, есть ли дочерние элементы
                    LinkedList childList = IedConnection_getDataDirectory(connection_, &error, attrRef.c_str());
                    if (childList) {
                        // Это структура
                        Napi::Value childAttrs = getAttributes(attrRef);
                        if (!childAttrs.IsNull()) {
                            Napi::Object attrInfo = Napi::Object::New(env);
                            attrInfo.Set("name", Napi::String::New(env, attrName));
                            attrInfo.Set("reference", Napi::String::New(env, attrRef));
                            attrInfo.Set("fc", Napi::Number::New(env, fc));
                            attrInfo.Set("isStructure", Napi::Boolean::New(env, true));
                            attrInfo.Set("attributes", childAttrs);
                            attributes.Set(attrName, attrInfo);
                        }
                        LinkedList_destroy(childList);
                    } else {
                        // Простой атрибут
                        Napi::Object attrInfo = Napi::Object::New(env);
                        attrInfo.Set("name", Napi::String::New(env, attrName));
                        attrInfo.Set("reference", Napi::String::New(env, attrRef));
                        attrInfo.Set("fc", Napi::Number::New(env, fc));
                        attrInfo.Set("isStructure", Napi::Boolean::New(env, false));
                        attributes.Set(attrName, attrInfo);
                    }
                }
                entry = LinkedList_getNext(entry);
            }
            LinkedList_destroy(attrList);
        }
        
        return attributes;
    };
    
    Napi::Value attributes = getAttributes(doRef);
    result.Set("attributes", attributes);
    
    return result;
}

// Функция для получения деталей DataSet
Napi::Value MmsClient::GetDataSetDetails(Napi::Env env, const std::string& dsRef) {
    printf("GetDataSetDetails: %s\n", dsRef.c_str());
    
    IedClientError error;
    bool isDeletable = false;
    
    Napi::Object result = Napi::Object::New(env);
    result.Set("type", Napi::String::New(env, "dataset"));
    result.Set("reference", Napi::String::New(env, dsRef));
    
    // Извлекаем имя DS из ссылки
    size_t dotPos = dsRef.find_last_of('.');
    if (dotPos != std::string::npos) {
        std::string dsName = dsRef.substr(dotPos + 1);
        result.Set("name", Napi::String::New(env, dsName));
    }
    
    // Проверяем, не закэширован ли уже этот DataSet
    {
        std::lock_guard<std::recursive_mutex> lock(connMutex_);
        if (datasetCache_.find(dsRef) != datasetCache_.end()) {
            printf("  DataSet %s already cached, skipping detailed caching\n", dsRef.c_str());
            
            // Возвращаем информацию из кэша
            DataSetCache& cache = datasetCache_[dsRef];
            Napi::Array memberArray = Napi::Array::New(env);
            
            for (size_t i = 0; i < cache.memberRefs.size(); ++i) {
                Napi::Object memberObj = Napi::Object::New(env);
                memberObj.Set("reference", Napi::String::New(env, cache.memberRefs[i]));
                
                // Извлекаем имя атрибута из ссылки
                size_t lastDot = cache.memberRefs[i].rfind('.');
                if (lastDot != std::string::npos) {
                    std::string attrName = cache.memberRefs[i].substr(lastDot + 1);
                    memberObj.Set("name", Napi::String::New(env, attrName));
                }
                
                memberArray.Set(i, memberObj);
            }
            
            result.Set("isDeletable", Napi::Boolean::New(env, false)); // Невозможно определить из кэша
            result.Set("members", memberArray);
            result.Set("memberCount", Napi::Number::New(env, cache.memberRefs.size()));
            result.Set("alreadyCached", Napi::Boolean::New(env, true));
            
            return result;
        }
    }
    
    // Получаем члены DataSet (только если не закэширован)
    LinkedList members = IedConnection_getDataSetDirectory(
        connection_, &error, dsRef.c_str(), &isDeletable);
    
    if (error != IED_ERROR_OK || !members) {
        printf("  ERROR: Cannot get dataset directory, error: %d\n", error);
        result.Set("isValid", false);
        result.Set("errorReason", Napi::String::New(env, "Cannot get dataset directory"));
        return result;
    }
    
    // Собираем ссылки на членов
    std::vector<std::string> memberRefs;
    LinkedList entry = members;
    int memberCount = 0;
    
    Napi::Array memberArray = Napi::Array::New(env);
    uint32_t memberIndex = 0;
    
    printf("  DataSet members:\n");
    while (entry) {
        if (entry->data) {
            char* memberRef = (char*)entry->data;
            std::string memberRefStr(memberRef);
            memberRefs.push_back(memberRefStr);
            
            printf("    [%d] %s\n", memberCount + 1, memberRef);
            
            Napi::Object memberObj = Napi::Object::New(env);
            memberObj.Set("reference", Napi::String::New(env, memberRefStr));
            
            // Извлекаем имя атрибута из ссылки
            size_t lastDot = memberRefStr.rfind('.');
            if (lastDot != std::string::npos) {
                std::string attrName = memberRefStr.substr(lastDot + 1);
                memberObj.Set("name", Napi::String::New(env, attrName));
            }
            
            memberArray.Set(memberIndex++, memberObj);
            memberCount++;
        }
        entry = LinkedList_getNext(entry);
    }
    
    LinkedList_destroy(members);
    
    result.Set("isDeletable", Napi::Boolean::New(env, isDeletable));
    result.Set("members", memberArray);
    result.Set("memberCount", Napi::Number::New(env, memberCount));
    result.Set("alreadyCached", Napi::Boolean::New(env, false));
    
    // КЭШИРУЕМ структуры для этого DataSet
    printf("  Caching structure for DataSet: %s (%d members)\n", dsRef.c_str(), memberCount);
    CacheDataSetStructure(dsRef, memberRefs);
    
    return result;
}

// Функция для получения деталей отчета
Napi::Value MmsClient::GetReportDetails(Napi::Env env, const std::string& rRef) {
    printf("GetReportDetails: %s\n", rRef.c_str());
    
    IedClientError error;
    Napi::Object result = Napi::Object::New(env);
    result.Set("type", Napi::String::New(env, "report"));
    result.Set("reference", Napi::String::New(env, rRef));
    
    // Извлекаем имя отчета из ссылки
    size_t dotPos = rRef.find_last_of('.');
    if (dotPos != std::string::npos) {
        std::string rName = rRef.substr(dotPos + 1);
        result.Set("name", Napi::String::New(env, rName));
    }
    
    // Определяем тип отчета (RP или BR)
    std::string reportType = "";
    if (rRef.find("RP$") != std::string::npos) {
        reportType = "RP";
    } else if (rRef.find("BR$") != std::string::npos) {
        reportType = "BR";
    }
    
    result.Set("reportType", Napi::String::New(env, reportType));
    
    // Получаем информацию о RCB
    ClientReportControlBlock rcb = nullptr;
    if (reportType == "RP") {
        rcb = IedConnection_getRCBValues(connection_, &error, rRef.c_str(), nullptr);
    } else if (reportType == "BR") {
        rcb = IedConnection_getRCBValues(connection_, &error, rRef.c_str(), nullptr);
    }
    
    if (error != IED_ERROR_OK || !rcb) {
        printf("  ERROR: Cannot get RCB values, error: %d\n", error);
        result.Set("isValid", false);
        result.Set("errorReason", Napi::String::New(env, "Cannot get RCB values"));
        return result;
    }
    
    // Получаем связанный DataSet
    const char* datasetRef = ClientReportControlBlock_getDataSetReference(rcb);
    if (datasetRef) {
        std::string datasetRefStr(datasetRef);
        
        // НОРМАЛИЗУЕМ имя DataSet: заменяем $ на .
        // В кэше используются имена с точкой, но сервер может возвращать с $
        std::string normalizedDatasetRef = datasetRefStr;
        std::replace(normalizedDatasetRef.begin(), normalizedDatasetRef.end(), '$', '.');
        
        printf("  Original dataset ref: %s\n", datasetRefStr.c_str());
        printf("  Normalized dataset ref: %s\n", normalizedDatasetRef.c_str());
        
        result.Set("datasetRef", Napi::String::New(env, normalizedDatasetRef));
        result.Set("originalDatasetRef", Napi::String::New(env, datasetRefStr));
        
        // Проверяем, не закэширован ли уже этот DataSet (по нормализованному имени)
        bool datasetAlreadyCached = false;
        {
            std::lock_guard<std::recursive_mutex> lock(connMutex_);
            datasetAlreadyCached = (datasetCache_.find(normalizedDatasetRef) != datasetCache_.end());
        }
        
        if (!datasetAlreadyCached) {
            printf("  Dataset %s not cached yet, caching now...\n", normalizedDatasetRef.c_str());
            
            // Получаем члены DataSet для кэширования (используем нормализованное имя)
            bool isDeletable = false;
            LinkedList members = IedConnection_getDataSetDirectory(
                connection_, &error, normalizedDatasetRef.c_str(), &isDeletable);
            
            if (error == IED_ERROR_OK && members) {
                // Собираем ссылки на членов
                std::vector<std::string> memberRefs;
                LinkedList entry = members;
                
                while (entry) {
                    if (entry->data) {
                        char* memberRef = (char*)entry->data;
                        memberRefs.push_back(std::string(memberRef));
                    }
                    entry = LinkedList_getNext(entry);
                }
                
                LinkedList_destroy(members);
                
                // КЭШИРУЕМ структуры для этого DataSet
                printf("  Caching structure for Report's DataSet: %s\n", normalizedDatasetRef.c_str());
                CacheDataSetStructure(normalizedDatasetRef, memberRefs);
            } else {
                printf("  WARNING: Failed to get DataSet directory for %s, error: %d\n", 
                       normalizedDatasetRef.c_str(), error);
                
                // Попробуем получить с оригинальным именем
                bool isDeletable2 = false;
                LinkedList members2 = IedConnection_getDataSetDirectory(
                    connection_, &error, datasetRefStr.c_str(), &isDeletable2);
                
                if (error == IED_ERROR_OK && members2) {
                    std::vector<std::string> memberRefs;
                    LinkedList entry = members2;
                    
                    while (entry) {
                        if (entry->data) {
                            char* memberRef = (char*)entry->data;
                            memberRefs.push_back(std::string(memberRef));
                        }
                        entry = LinkedList_getNext(entry);
                    }
                    
                    LinkedList_destroy(members2);
                    
                    printf("  Caching structure for Report's DataSet (original name): %s\n", normalizedDatasetRef.c_str());
                    CacheDataSetStructure(normalizedDatasetRef, memberRefs);
                }
            }
        } else {
            printf("  Dataset %s already cached, skipping\n", normalizedDatasetRef.c_str());
            result.Set("datasetAlreadyCached", Napi::Boolean::New(env, true));
        }
    }
    
    // Получаем состояние отчета
    bool rptEna = ClientReportControlBlock_getRptEna(rcb);
    result.Set("enabled", Napi::Boolean::New(env, rptEna));
    
    // Получаем Report ID
    const char* rptId = ClientReportControlBlock_getRptId(rcb);
    if (rptId) {
        result.Set("reportId", Napi::String::New(env, rptId));
    }
    
    // Получаем другие параметры отчета
    int trgOps = ClientReportControlBlock_getTrgOps(rcb);
    int intgPd = ClientReportControlBlock_getIntgPd(rcb);
    int bufTm = ClientReportControlBlock_getBufTm(rcb);
    bool gi = ClientReportControlBlock_getGI(rcb);
    
    result.Set("trgOps", Napi::Number::New(env, trgOps));
    result.Set("intgPd", Napi::Number::New(env, intgPd));
    result.Set("bufTm", Napi::Number::New(env, bufTm));
    result.Set("gi", Napi::Boolean::New(env, gi));
    
    ClientReportControlBlock_destroy(rcb);
    result.Set("isValid", true);
    
    return result;
}

Napi::Value MmsClient::BrowseDataModel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    // Если параметр не передан - возвращаем только корневые узлы
    if (info.Length() == 0) {
        return GetRootNodes(env);
    }
    
    // Если передан параметр
    if (info.Length() >= 1 && info[0].IsString()) {
        std::string ref = info[0].As<Napi::String>().Utf8Value();
        return BrowseSpecificObject(env, ref);
    }
    
    Napi::TypeError::New(env, "Expected string parameter or no parameters").ThrowAsJavaScriptException();
    return env.Null();
}

Napi::Value MmsClient::CreateDataSet(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
        Napi::TypeError::New(env, "Expected datasetRef (string) and dataSetElements (array)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::string datasetRef = info[0].As<Napi::String>().Utf8Value();
    Napi::Array elements = info[1].As<Napi::Array>();
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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

    if (info.Length() < 1 || !info[0].IsString()) {
        printf("GetDataSetDirectory: Invalid input, expected logicalNodeRef (string), clientID: %s\n", clientID_.c_str());
        deferred.Reject(Napi::TypeError::New(env, "Expected logicalNodeRef (string)").Value());
        return deferred.Promise();
    }

    std::string logicalNodeRef = info[0].As<Napi::String>().Utf8Value();
    printf("GetDataSetDirectory: Attempting to retrieve datasets for %s, clientID: %s\n", 
           logicalNodeRef.c_str(), clientID_.c_str());

    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    if (!connected_) {
        printf("GetDataSetDirectory: Not connected, clientID: %s\n", clientID_.c_str());
        deferred.Reject(Napi::Error::New(env, "Not connected").Value());
        return deferred.Promise();
    }

    try {
        IedClientError error;
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

    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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
        
        std::string actualRef = ref;
        FunctionalConstraint fc = IEC61850_FC_ST;
        
        // Извлекаем FC из ссылки
        size_t bracketPos = ref.find('[');
        if (bracketPos != std::string::npos && ref.back() == ']') {
            std::string fcStr = ref.substr(bracketPos + 1, ref.length() - bracketPos - 2);
            fc = ParseFCFromString(fcStr);
            actualRef = ref.substr(0, bracketPos);
            
            //printf("ReadData: Parsed ref '%s' -> actualRef='%s', fc=%d\n", ref.c_str(), actualRef.c_str(), fc);
        }
        
        // Пытаемся прочитать значение с указанным FC
        value = IedConnection_readObject(connection_, &error, actualRef.c_str(), fc);
        
        // Если не получилось, пробуем альтернативные FC
        if (error != IED_ERROR_OK || !value) {
            std::vector<FunctionalConstraint> fcs = {
                IEC61850_FC_ST, IEC61850_FC_MX, IEC61850_FC_CO,
                IEC61850_FC_CF, IEC61850_FC_DC, IEC61850_FC_SP,
                IEC61850_FC_SG, IEC61850_FC_ALL
            };
            
            for (auto tryFc : fcs) {
                if (tryFc == fc) continue; // Уже пробовали
                
                if (value) {
                    MmsValue_delete(value);
                    value = nullptr;
                }
                
                value = IedConnection_readObject(connection_, &error, actualRef.c_str(), tryFc);
                if (error == IED_ERROR_OK && value) {
                    fc = tryFc;
                    //printf("ReadData: Success with alternative FC=%d for '%s'\n", fc, actualRef.c_str());
                    break;
                }
            }
        }

        if (error != IED_ERROR_OK || !value) {          
            item.Set("isValid", false);
            std::string errorReason;
            switch (error) {
                case IED_ERROR_OBJECT_DOES_NOT_EXIST:
                    errorReason = "Object does not exist: " + ref;
                    break;
                case IED_ERROR_ACCESS_DENIED:
                    errorReason = "Access denied: " + ref;
                    break;
                case IED_ERROR_TYPE_INCONSISTENT:
                    errorReason = "Type inconsistent: " + ref;
                    break;
                case IED_ERROR_OBJECT_ACCESS_UNSUPPORTED:
                    errorReason = "Object access unsupported: " + ref;
                    break;
                default:
                    errorReason = "Read failed for " + ref + ": error " + std::to_string(error);
                    break;
            }
            item.Set("errorReason", Napi::String::New(env, errorReason));
            item.Set("value", Napi::String::New(env, errorReason)); // Возвращаем ошибку как значение
            
            printf("ReadData: Failed to read %s, error: %d, reason: %s\n", 
                   ref.c_str(), error, errorReason.c_str());
        } else {
            // Проверяем, не является ли значение ошибкой доступа
            int type = MmsValue_getType(value);
            
            if (type == MMS_DATA_ACCESS_ERROR) {
                item.Set("isValid", false);
                item.Set("errorReason", Napi::String::New(env, "Data access error for " + ref));
                item.Set("value", Napi::String::New(env, "Data access error"));
            } else {
                // Извлекаем имя атрибута
                std::string attrName = actualRef;
                size_t lastDot = actualRef.rfind('.');
                if (lastDot != std::string::npos) {
                    attrName = actualRef.substr(lastDot + 1);
                }
                
                // Для атрибутов stVal добавляем [ST] если его нет
                if ((attrName.find("stVal") != std::string::npos || attrName.find("Pos") != std::string::npos) &&
                    ref.find('[') == std::string::npos) {
                    attrName += "[ST]";
                }
                
                item.Set("isValid", true);
                item.Set("value", SafeConvertMmsValue(env, connection_, this, ref, value, attrName, 0));
                
                printf("ReadData: Successfully read %s, type: %d\n", ref.c_str(), type);
            }
            
            MmsValue_delete(value);
        }

        results.Set(static_cast<uint32_t>(i), item);
    }

    return results;
}

Napi::Value MmsClient::ControlObject(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBoolean()) {
        Napi::TypeError::New(env, "Expected dataRef (string) and value (boolean)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string dataRef = info[0].As<Napi::String>().Utf8Value();
    bool controlValue = info[1].As<Napi::Boolean>().Value();

    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    if (!connected_) {
        printf("ControlObject: Not connected, clientID: %s\n", clientID_.c_str());
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        IedClientError error;

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
            ctlModel = 1;
        }

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

        std::string operRef = dataRef + ".Oper";
        std::string stValRef = dataRef + ".stVal";
        printf("Attempting control on: %s\n", operRef.c_str());

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

        if (ctlModel == 1) {
            printf("Using DIRECT control (ctlModel=%d)\n", ctlModel);
            ControlObjectClient_setOrigin(control, NULL, 3);
            operateSuccess = ControlObjectClient_operate(control, ctlVal, 1);
        }
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
        else if (ctlModel == 3) {
            printf("Using DIRECT control with enhanced security (ctlModel=%d)\n", ctlModel);
            ControlObjectClient_setCommandTerminationHandler(control, commandTerminationHandler, this);
            ControlObjectClient_setOrigin(control, nullptr, 3);
            operateSuccess = ControlObjectClient_operate(control, ctlVal, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        else if (ctlModel == 4) {
            printf("Using SBO control with enhanced security (ctlModel=%d)\n", ctlModel);
            ControlObjectClient_setCommandTerminationHandler(control, commandTerminationHandler, this);
            if (ControlObjectClient_selectWithValue(control, ctlVal)) {
                operateSuccess = ControlObjectClient_operate(control, ctlVal, 0);
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

        MmsValue_delete(ctlVal);
        ControlObjectClient_destroy(control);

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
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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
                            dataObj.subObjects.push_back(subObj);
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
        
        LinkedList deviceList = IedConnection_getLogicalDeviceList(connection_, &error);
        if (error != IED_ERROR_OK || deviceList == nullptr) {
            printf("Failed to get logical device list, error: %d, clientID: %s\n", error, clientID_.c_str());
                        
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
                                ln.dataObjects.push_back(dataObj);
                            }
                            currentDo = LinkedList_getNext(currentDo);
                        }
                        LinkedList_destroy(doList);
                        ld.logicalNodes.push_back(ln);
                    }
                    currentNode = LinkedList_getNext(currentNode);
                }
                LinkedList_destroy(nodeList);
                logicalDevices.push_back(ld);
            }
            currentDevice = LinkedList_getNext(currentDevice);
        }
        LinkedList_destroy(deviceList);
        if (logicalDevices.empty()) {
            printf("No valid logical devices found, clientID: %s\n", clientID_.c_str());
                        
            deferred.Reject(Napi::Error::New(env, "No valid logical devices found").Value());
            return deferred.Promise();
        }             
        
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
        printf("GetLogicalDevices: Successfully retrieved %zu logical devices, clientID: %s\n", 
               logicalDevices.size(), clientID_.c_str());
        deferred.Resolve(resultArray);
        return deferred.Promise();
    } catch (const std::exception& e) {
        printf("GetLogicalDevices: Exception occurred: %s, clientID: %s\n", e.what(), clientID_.c_str());
         
        deferred.Reject(Napi::Error::New(env, std::string("Exception in GetLogicalDevices: ") + e.what()).Value());
        return deferred.Promise();
    }
}

Napi::Value MmsClient::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    Napi::Object status = Napi::Object::New(env);
    status.Set("connected", Napi::Boolean::New(env, connected_));
    status.Set("clientID", Napi::String::New(env, clientID_.c_str()));
    return status;
}

// Функция для преобразования ResultData в NAPI значение с использованием имен
static Napi::Value ResultDataToNapiWithNames(Napi::Env env, 
                                            const MmsClient::ResultData& data, 
                                            const std::string& attrName) {
    try {
        if (!data.isValid) {
            return Napi::String::New(env, data.errorReason);
        }
        
        switch (data.type) {
            case MMS_FLOAT:
                return Napi::Number::New(env, data.floatValue);
                
            case MMS_INTEGER:
            case MMS_UNSIGNED:
                if (!data.stringValue.empty()) {
                    return Napi::String::New(env, data.stringValue);
                }
                return Napi::Number::New(env, static_cast<double>(data.intValue));
                
            case MMS_BOOLEAN:
                return Napi::Boolean::New(env, data.boolValue);
                
            case MMS_VISIBLE_STRING:
                return Napi::String::New(env, data.stringValue);
                
            case MMS_UTC_TIME:
                return Napi::Number::New(env, static_cast<double>(data.intValue));
                
            case MMS_BIT_STRING:
                if (!data.stringValue.empty()) {
                    return Napi::String::New(env, data.stringValue);
                }
                return Napi::Number::New(env, static_cast<double>(data.intValue));
                
            case MMS_STRUCTURE: {
                // ЕСЛИ У НАС ЕСТЬ КЭШИРОВАННЫЕ ИМЕНА - ИСПОЛЬЗУЕМ ИХ
                if (!data.structureElementNames.empty() && 
                    data.structureElementNames.size() == data.structureElements.size()) {
                    
                    Napi::Object structObj = Napi::Object::New(env);
                    
                    for (size_t i = 0; i < data.structureElements.size(); ++i) {
                        const std::string& elementName = data.structureElementNames[i];
                        const MmsClient::ResultData& elementData = data.structureElements[i];
                        
                        // Для вложенных структур передаем имя элемента
                        structObj.Set(elementName,
                                    ResultDataToNapiWithNames(env, elementData, elementName));
                    }
                    return structObj;
                } else {
                    // НЕТ КЭШИРОВАННЫХ ИМЕН - используем числовые индексы
                    Napi::Object structObj = Napi::Object::New(env);
                    
                    for (size_t i = 0; i < data.structureElements.size(); ++i) {
                        std::string indexName = std::to_string(i);
                        structObj.Set(indexName,
                                    ResultDataToNapiWithNames(env, data.structureElements[i], indexName));
                    }
                    return structObj;
                }
            }
            
            case MMS_ARRAY: {
                Napi::Array arr = Napi::Array::New(env, data.arrayElements.size());
                for (size_t i = 0; i < data.arrayElements.size(); ++i) {
                    arr.Set(i, ResultDataToNapiWithNames(env, data.arrayElements[i], attrName));
                }
                return arr;
            }
            
            default:
                return Napi::String::New(env, "type_" + std::to_string(data.type));
        }
    } catch (const std::exception& e) {
        printf("ResultDataToNapiWithNames std::exception: %s for %s\n", e.what(), attrName.c_str());
        return Napi::String::New(env, "Conversion Error");
    } catch (...) {
        printf("ResultDataToNapiWithNames unknown exception for %s\n", attrName.c_str());
        return Napi::String::New(env, "Unknown Error");
    }
}

void MmsClient::ReportCallback(void* parameter, ClientReport report) {
    MmsClient* client = static_cast<MmsClient*>(parameter);
    
    uint64_t callbackEntryTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    auto startTime = std::chrono::steady_clock::now();
    MmsClient::totalReportsProcessed_++;
    
    int currentReport = MmsClient::totalReportsProcessed_.load();
    
    // Получаем идентификатор потока
    std::thread::id threadId = std::this_thread::get_id();
    size_t threadIdHash = std::hash<std::thread::id>{}(threadId);
    
    printf("\n=== ReportCallback [REPORT#%d] ===\n", currentReport);
    printf("  Callback entry time: %llu ms\n", callbackEntryTime);
    printf("  Thread ID: %zu (0x%zx)\n", threadIdHash, threadIdHash);
    printf("  clientID: %s\n", client->clientID_.c_str());
    printf("  client pointer: %p\n", (void*)client);
    
    // Быстрая проверка состояния клиента
    bool isClosing = false;
    bool isConnected = false;
    
    uint64_t stateCheckStart = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    {
        std::lock_guard<std::recursive_mutex> lock(client->connMutex_);
        uint64_t lockAcquired = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        if (lockAcquired - stateCheckStart > 10) {
            printf("  WARNING: State check lock acquisition took %llu ms\n", 
                   lockAcquired - stateCheckStart);
        }
        
        isClosing = client->isClosing_;
        isConnected = client->connected_;
        
        printf("  State check lock acquired at %llu ms (waited %llu ms)\n", 
               lockAcquired, lockAcquired - stateCheckStart);
        printf("  isClosing = %d, isConnected = %d\n", isClosing, isConnected);
    }
    
    if (isClosing) {
        printf("  Client is closing, skipping report\n");
        return;
    }
    
    if (!isConnected) {
        printf("  Client not connected, skipping report\n");
        return;
    }
    
    // Получаем информацию об отчете
    const char* rcbRefRaw = ClientReport_getRcbReference(report);
    const char* rptIdRaw = ClientReport_getRptId(report);
    std::string rcbRef = rcbRefRaw ? rcbRefRaw : "unknown";
    std::string rptId = rptIdRaw ? rptIdRaw : "unknown";
    
    printf("  Report for RCB: %s, rptId: %s\n", rcbRef.c_str(), rptId.c_str());
    
    // Получаем значения DataSet
    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);
    
    if (!dataSetValues) {
        printf("  ERROR: dataSetValues is NULL\n");
        return;
    }

    int dataSetSize = MmsValue_getArraySize(dataSetValues);
    int dataSetType = MmsValue_getType(dataSetValues);
    printf("  dataSetValues: type=%d, array size=%d\n", dataSetType, dataSetSize);
    
    // Получаем информацию об отчете с блокировкой
    ReportInfo* reportInfo = nullptr;
    std::vector<std::string> dataSetMembers;
    
    uint64_t reportInfoLockStart = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    {
        std::lock_guard<std::recursive_mutex> lock(client->connMutex_);
        uint64_t reportInfoLockAcquired = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        if (reportInfoLockAcquired - reportInfoLockStart > 10) {
            printf("  WARNING: ReportInfo lock acquisition took %llu ms\n", 
                   reportInfoLockAcquired - reportInfoLockStart);
        }
        
        printf("  ReportInfo lock acquired at %llu ms (waited %llu ms)\n", 
               reportInfoLockAcquired, reportInfoLockAcquired - reportInfoLockStart);
        
        auto it = client->activeReports_.find(rcbRef);
        if (it != client->activeReports_.end()) {
            reportInfo = &it->second;
            dataSetMembers = reportInfo->dataSetMembers; // копируем
            printf("  Found active report, dataSet members: %zu, reportInfo ptr: %p\n", 
                   dataSetMembers.size(), (void*)reportInfo);
            printf("  structureElementNamesCache size: %zu\n", 
                   reportInfo->structureElementNamesCache.size());
        } else {
            printf("  WARNING: ReportInfo not found for %s\n", rcbRef.c_str());
            printf("  activeReports_ size: %zu\n", client->activeReports_.size());
            return;
        }
    }
    
    if (dataSetMembers.size() != static_cast<size_t>(dataSetSize)) {
        printf("  WARNING: Members count (%zu) != DataSet size (%d)\n", 
               dataSetMembers.size(), dataSetSize);
    }
    
    // Ограничиваем обработку для больших отчетов
    const int MAX_ELEMENTS_TO_PROCESS = 200;
    int elementsToProcess = std::min(dataSetSize, MAX_ELEMENTS_TO_PROCESS);
    
    bool hasTimestamp = ClientReport_hasTimestamp(report);
    uint64_t timestamp = 0;
    if (hasTimestamp) {
        timestamp = ClientReport_getTimestamp(report);
        printf("  Report has timestamp: %llu ms\n", (unsigned long long)timestamp);
    }
    
    // Структура для хранения обработанных данных
    struct ReportItemData {
        std::string fullRef;
        MmsClient::ResultData resultData;
        int reason;
    };
    
    std::vector<ReportItemData> reportItems;
    reportItems.reserve(elementsToProcess);
    
    printf("  Starting data processing at %llu ms\n", 
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Вспомогательная функция для обработки структурных значений
    std::function<MmsClient::ResultData(MmsValue*, const std::string&, int)> processValueRecursive;
    processValueRecursive = [&](MmsValue* val, const std::string& fullRef, int recursionDepth) -> MmsClient::ResultData {
        MmsClient::ResultData data;
        
        const int MAX_RECURSION_DEPTH = 5;
        if (recursionDepth > MAX_RECURSION_DEPTH) {
            data.type = MMS_STRUCTURE;
            data.isValid = false;
            data.errorReason = "Max recursion depth exceeded";
            return data;
        }
        
        if (!val) {
            data.type = MMS_DATA_ACCESS_ERROR;
            data.isValid = false;
            data.errorReason = "Null value";
            return data;
        }
        
        data.type = MmsValue_getType(val);
        data.isValid = true;
        data.errorReason = "";
        
        // Извлекаем имя атрибута из полной ссылки
        std::string attrName = fullRef;
        size_t dotPos = fullRef.rfind('.');
        if (dotPos != std::string::npos) {
            attrName = fullRef.substr(dotPos + 1);
        }
        
        // Проверяем, является ли это структурой статуса [ST]
        bool isStatusStructure = false;
        size_t bracketPos = fullRef.find('[');
        if (bracketPos != std::string::npos) {
            std::string fcPart = fullRef.substr(bracketPos);
            if (fcPart.find("[ST]") != std::string::npos || fcPart.find("[st]") != std::string::npos) {
                isStatusStructure = true;
            }
        }
        
        if (data.type == MMS_STRUCTURE) {
            int size = MmsValue_getArraySize(val);
            
            // Для структур статуса [ST] используем стандартные имена
            if (isStatusStructure && size >= 3) {
                const char* stdNames[] = {"stVal", "q", "t"};
                for (int i = 0; i < std::min(size, 3); ++i) {
                    MmsValue* childVal = MmsValue_getElement(val, i);
                    if (childVal) {
                        std::string childFullRef = fullRef;
                        if (bracketPos != std::string::npos) {
                            childFullRef = fullRef.substr(0, bracketPos) + "." + stdNames[i] + 
                                         fullRef.substr(bracketPos);
                        } else {
                            childFullRef = fullRef + "." + stdNames[i];
                        }
                        MmsClient::ResultData childData = processValueRecursive(childVal, childFullRef, recursionDepth + 1);
                        data.structureElements.push_back(childData);
                        data.structureElementNames.push_back(stdNames[i]);
                    }
                }
            } else {
                // Для нестандартных структур используем индексы
                for (int i = 0; i < size; ++i) {
                    MmsValue* childVal = MmsValue_getElement(val, i);
                    if (childVal) {
                        std::string indexName = std::to_string(i);
                        std::string childFullRef = fullRef + "." + indexName;
                        MmsClient::ResultData childData = processValueRecursive(childVal, childFullRef, recursionDepth + 1);
                        data.structureElements.push_back(childData);
                        data.structureElementNames.push_back(indexName);
                    }
                }
            }
        } else {
            // Для простых типов используем ConvertMmsValueForReportFast
            data = ConvertMmsValueForReportFast(val, attrName);
        }
        return data;
    };
    
    // Обрабатываем данные
    int itemsProcessed = 0;
    for (int i = 0; i < elementsToProcess; i++) {
        ReasonForInclusion reason = ClientReport_getReasonForInclusion(report, i);
        if (reason == IEC61850_REASON_NOT_INCLUDED) {
            continue;
        }
        
        std::string fullRef;
        if (i < static_cast<int>(dataSetMembers.size())) {
            fullRef = dataSetMembers[i];
        } else {
            fullRef = "unknown[" + std::to_string(i) + "]";
        }
        
        MmsValue* value = MmsValue_getElement(dataSetValues, i);
        if (!value) {
            printf("    [%d] WARNING: value is NULL for %s\n", i, fullRef.c_str());
            continue;
        }
        
        try {
            uint64_t itemStart = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            MmsClient::ResultData rd = processValueRecursive(value, fullRef, 0);
            
            uint64_t itemEnd = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            ReportItemData item;
            item.fullRef = fullRef;
            item.resultData = std::move(rd);
            item.reason = reason;
            reportItems.push_back(std::move(item));
            itemsProcessed++;
            
            if (itemsProcessed <= 5) { // Логируем только первые 5 элементов для отладки
                printf("    [%d] %s processed in %llu µs, type=%d\n", 
                       i, fullRef.c_str(), itemEnd - itemStart, item.resultData.type);
            }
        } catch (const std::exception& e) {
            printf("    [%d] Exception in processValueRecursive for %s: %s\n", 
                   i, fullRef.c_str(), e.what());
        }
    }
    
    uint64_t processingEnd = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    printf("  Data processing completed at %llu ms\n", processingEnd);
    printf("  Processed %zu items from report (%d items with data)\n", 
           reportItems.size(), itemsProcessed);
    
    MmsClient::totalElementsProcessed_ += reportItems.size();
    
    // === Применяем кэшированные имена структур (под мьютексом) ===
    if (reportInfo && !reportItems.empty()) {
        uint64_t enhanceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        {
            std::lock_guard<std::recursive_mutex> lock(client->connMutex_);
            uint64_t enhanceLockAcquired = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            printf("  EnhanceStructure lock acquired at %llu ms (waited %llu ms)\n", 
                   enhanceLockAcquired, enhanceLockAcquired - enhanceStart);
            
            int enhancedCount = 0;
            for (auto& item : reportItems) {
                if (item.resultData.type == MMS_STRUCTURE) {
                    EnhanceStructureWithCachedNames(item.resultData, item.fullRef, *reportInfo);
                    enhancedCount++;
                }
            }
            printf("  Enhanced %d structures with cached names\n", enhancedCount);
        }
        
        uint64_t enhanceEnd = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        printf("  Structure enhancement completed in %llu ms\n", enhanceEnd - enhanceStart);
    }
    
    // Отправляем в JS если есть что отправлять
    if (!reportItems.empty() && client->tsfn_) {
        printf("  Sending report event to JS...\n");
        
        // Проверяем состояние соединения перед отправкой
        {
            std::lock_guard<std::recursive_mutex> lock(client->connMutex_);
            if (!client->connected_ || client->isClosing_) {
                printf("  Client disconnected or closing, skipping send to JS\n");
                return;
            }
        }
        
        uint64_t tsfnStart = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        // Передаём данные в основной поток (копируем reportItems)
        auto status = client->tsfn_.NonBlockingCall([client, rcbRef, rptId, timestamp, hasTimestamp, reportItems, currentReport]
                                                    (Napi::Env env, Napi::Function cb) {
            uint64_t tsfnEntry = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            printf("  [TSFN #%d] Entered at %llu ms, thread ID: %zu\n", 
                   currentReport, tsfnEntry, 
                   std::hash<std::thread::id>{}(std::this_thread::get_id()));
            
            try {
                // Проверяем, не закрывается ли клиент
                {
                    std::lock_guard<std::recursive_mutex> lock(client->connMutex_);
                    if (client->isClosing_) {
                        printf("  [TSFN #%d] Skipping report - client is closing\n", currentReport);
                        return;
                    }
                }
                
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, client->clientID_.c_str()));
                eventObj.Set("type", "data");
                eventObj.Set("event", "report");
                eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
                eventObj.Set("rptId", Napi::String::New(env, rptId));
                
                if (hasTimestamp) {
                    eventObj.Set("timestamp", Napi::Number::New(env, static_cast<double>(timestamp)));
                }
                
                Napi::Object valuesObj = Napi::Object::New(env);
                Napi::Object reasonsObj = Napi::Object::New(env);
                
                int itemCount = 0;
                for (const auto& item : reportItems) {
                    try {
                        // item.resultData уже содержит имена элементов (structureElementNames)
                        Napi::Value jsValue = ResultDataToNapiWithNames(env, item.resultData, item.fullRef);
                        valuesObj.Set(item.fullRef, jsValue);
                        reasonsObj.Set(item.fullRef, item.reason);
                        itemCount++;
                    } catch (const std::exception& e) {
                        printf("  [TSFN #%d] Exception converting item %s: %s\n", 
                               currentReport, item.fullRef.c_str(), e.what());
                        valuesObj.Set(item.fullRef, Napi::String::New(env, "Conversion Exception"));
                        reasonsObj.Set(item.fullRef, item.reason);
                    }
                }
                
                eventObj.Set("values", valuesObj);
                eventObj.Set("reasons", reasonsObj);
                eventObj.Set("reportNumber", Napi::Number::New(env, currentReport));
                eventObj.Set("itemsInReport", Napi::Number::New(env, itemCount));
                eventObj.Set("totalElementsProcessed", Napi::Number::New(env, MmsClient::totalElementsProcessed_.load()));
                
                uint64_t tsfnCallStart = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                
                cb.Call({Napi::String::New(env, "data"), eventObj});
                
                uint64_t tsfnCallEnd = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                
                printf("  [TSFN #%d] JS callback executed in %llu ms\n", 
                       currentReport, tsfnCallEnd - tsfnCallStart);
                
            } catch (const std::exception& e) {
                printf("  [TSFN #%d] std::exception: %s\n", currentReport, e.what());
            } catch (...) {
                printf("  [TSFN #%d] Unknown exception\n", currentReport);
            }
            
            uint64_t tsfnExit = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            printf("  [TSFN #%d] Exited at %llu ms (total time in TSFN: %llu ms)\n", 
                   currentReport, tsfnExit, tsfnExit - tsfnEntry);
        });
        
        uint64_t tsfnEnd = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        if (status != napi_ok) {
            printf("  ERROR: Failed to queue report to TSFN, status: %d\n", status);
        } else {
            printf("  Report queued to TSFN successfully at %llu ms (queue time: %llu ms)\n", 
                   tsfnEnd, tsfnEnd - tsfnStart);
        }
    } else {
        if (reportItems.empty()) {
            printf("  No valid items to send to JS (reportItems empty)\n");
        }
        if (!client->tsfn_) {
            printf("  TSFN is null, cannot send to JS\n");
        }
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    uint64_t callbackExitTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    printf("  Report total processing time: %lld ms\n", duration.count());
    printf("  Callback total time: %llu ms (from entry to exit)\n", 
           callbackExitTime - callbackEntryTime);
    printf("=== ReportCallback [REPORT#%d] END ===\n\n", currentReport);
}

Napi::Value MmsClient::EnableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string) and datasetRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();
    std::string datasetRef = info[1].As<Napi::String>().Utf8Value();
    
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    if (!connected_ || !connection_) {
        printf("EnableReporting: Not connected or connection invalid\n");
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (activeReports_.find(rcbRef) != activeReports_.end()) {
        printf("EnableReporting: Report already enabled for %s\n", rcbRef.c_str());
        Napi::Error::New(env, "Report already enabled for " + rcbRef).ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        IedClientError error;

        // 1. Получаем DataSet Directory
        bool isDeletable = false;
        printf("EnableReporting: Getting dataset directory for %s\n", datasetRef.c_str());
        
        LinkedList dataSetDirectory = IedConnection_getDataSetDirectory(
            connection_, &error, datasetRef.c_str(), &isDeletable);
        
        if (error != IED_ERROR_OK || !dataSetDirectory) {
            printf("EnableReporting: Failed to get dataset directory for %s, error: %d\n", 
                   datasetRef.c_str(), error);
            Napi::Error::New(env, "Failed to get dataset directory: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        
        // Копируем данные из LinkedList в вектор
        std::vector<std::string> dataSetMembers;
        LinkedList entry = dataSetDirectory;
        int memberCount = 0;
        while (entry) {
            if (entry->data) {
                char* memberRef = (char*)entry->data;
                dataSetMembers.push_back(std::string(memberRef));
                memberCount++;
                
                if (memberCount <= 5) {
                    printf("EnableReporting: Member [%d]: %s\n", memberCount-1, memberRef);
                }
            }
            entry = LinkedList_getNext(entry);
        }
        
        LinkedList_destroy(dataSetDirectory);
        
        printf("EnableReporting: Got dataset directory for %s, count: %d\n", datasetRef.c_str(), memberCount);
        
        // 2. Проверяем, не закэширован ли уже этот DataSet
        bool datasetAlreadyCached = (datasetCache_.find(datasetRef) != datasetCache_.end());
        
        if (!datasetAlreadyCached) {
            printf("EnableReporting: DataSet %s not cached, caching now...\n", datasetRef.c_str());
            
            ReportInfo reportInfo;
            reportInfo.rcbRef = rcbRef;
            reportInfo.datasetRef = datasetRef;
            reportInfo.dataSetMembers = dataSetMembers;
            
            // Рекурсивно кэшируем структуры для всех членов DataSet
            for (const auto& memberRef : dataSetMembers) {
                std::string cleanRef = memberRef;
                FunctionalConstraint fc = IEC61850_FC_ST;
                
                size_t bracketPos = memberRef.find('[');
                if (bracketPos != std::string::npos && memberRef.back() == ']') {
                    std::string fcStr = memberRef.substr(bracketPos + 1, 
                                                        memberRef.length() - bracketPos - 2);
                    cleanRef = memberRef.substr(0, bracketPos);
                    fc = ParseFCFromString(fcStr);
                }
                
                CacheStructureElementNames(connection_, this, reportInfo, cleanRef, fc, 0);
            }
            
            // Основное кэширование структур
            CacheDataSetStructure(datasetRef, dataSetMembers);
        } else {
            printf("EnableReporting: DataSet %s already cached, using existing cache\n", datasetRef.c_str());
        }
        
        // 3. Получаем RCB
        printf("EnableReporting: Getting RCB values for %s\n", rcbRef.c_str());
        ClientReportControlBlock rcb = IedConnection_getRCBValues(
            connection_, &error, rcbRef.c_str(), nullptr);
        
        if (error != IED_ERROR_OK || !rcb) {
            printf("EnableReporting: Failed to get RCB %s, error: %d\n", 
                   rcbRef.c_str(), error);
            LogNetworkErrorDetailed(error);
            Napi::Error::New(env, "Failed to get RCB: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        
        // 3. Читаем DataSet
        printf("EnableReporting: Reading dataset values for %s\n", datasetRef.c_str());
        ClientDataSet clientDataSet = IedConnection_readDataSetValues(
            connection_, &error, datasetRef.c_str(), nullptr);
        
        if (error != IED_ERROR_OK || !clientDataSet) {
            printf("EnableReporting: Failed to read dataset %s, error: %d\n", 
                   datasetRef.c_str(), error);
            ClientReportControlBlock_destroy(rcb);
            Napi::Error::New(env, "Failed to read dataset: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        printf("EnableReporting: Read dataset for %s successfully\n", datasetRef.c_str());

        // 4. КЭШИРУЕМ ИМЕНА ЭЛЕМЕНТОВ СТРУКТУР (только если еще не закэшировано)
        printf("\n=== Caching structure element names for dataset %s ===\n", datasetRef.c_str());
        
        ReportInfo reportInfo;
        reportInfo.rcbRef = rcbRef;
        reportInfo.datasetRef = datasetRef;
        reportInfo.dataSetMembers = dataSetMembers;
        
        // Проверяем, нужно ли кэшировать имена элементов структуры
        // В отличие от datasetCache_, reportInfo.structureElementNamesCache может быть пустым
        // даже если datasetCache_ содержит данные, поэтому мы все равно вызываем CacheStructureElementNames
        // но передаем существующий reportInfo
        
        // Рекурсивно кэшируем структуры для всех членов DataSet
        for (const auto& memberRef : dataSetMembers) {
            std::string cleanRef = memberRef;
            FunctionalConstraint fc = IEC61850_FC_ST;
            
            size_t bracketPos = memberRef.find('[');
            if (bracketPos != std::string::npos && memberRef.back() == ']') {
                std::string fcStr = memberRef.substr(bracketPos + 1, 
                                                    memberRef.length() - bracketPos - 2);
                cleanRef = memberRef.substr(0, bracketPos);
                fc = ParseFCFromString(fcStr);
            }
            
            CacheStructureElementNames(connection_, this, reportInfo, cleanRef, fc, 0);
        }
        
        printf("=== Finished caching structure names for reporting ===\n\n");

        // 5. Настраиваем RCB
        // Сначала снимаем резервацию, если она установлена
        if (ClientReportControlBlock_getResv(rcb)) {
            printf("EnableReporting: RCB is reserved, trying to release reservation\n");
            ClientReportControlBlock_setResv(rcb, false);
            IedConnection_setRCBValues(connection_, &error, rcb, RCB_ELEMENT_RESV, true);
            if (error != IED_ERROR_OK) {
                printf("EnableReporting: Warning: Failed to release reservation, error: %d\n", error);
            }
        }
        
        // Устанавливаем ссылку на DataSet
        ClientReportControlBlock_setDataSetReference(rcb, datasetRef.c_str());
        
        // Устанавливаем триггеры
        ClientReportControlBlock_setTrgOps(rcb, 
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_INTEGRITY);
        
        // Устанавливаем период целостности
        ClientReportControlBlock_setIntgPd(rcb, 10000); // 10 секунд

        ClientReportControlBlock_setBufTm(rcb, 5000);  // 5 секунд буферного времени
        
        // Отключаем GI (General Interrogation) для начала
        ClientReportControlBlock_setGI(rcb, false);
        
        // Включаем отчет
        ClientReportControlBlock_setRptEna(rcb, true);
        
        // Пробуем установить значения RCB в несколько этапов
        printf("EnableReporting: Setting RCB values step by step...\n");
        
        // Этап 1: Устанавливаем только основные параметры без резервации
        uint32_t mask = RCB_ELEMENT_DATSET | RCB_ELEMENT_TRG_OPS | RCB_ELEMENT_INTG_PD;
        IedConnection_setRCBValues(connection_, &error, rcb, mask, true);
        
        if (error != IED_ERROR_OK) {
            printf("EnableReporting: Step 1 failed, error: %d\n", error);
            LogNetworkErrorDetailed(error);
            
            // Пробуем более простой подход: только включение отчета
            printf("EnableReporting: Trying minimal setup (only enable report)...\n");
            ClientReportControlBlock_setRptEna(rcb, false); // Сначала выключаем
            IedConnection_setRCBValues(connection_, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
            
            if (error == IED_ERROR_OK) {
                // Теперь включаем с минимальными параметрами
                ClientReportControlBlock_setRptEna(rcb, true);
                IedConnection_setRCBValues(connection_, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
            }
        }
        
        if (error != IED_ERROR_OK) {
            printf("EnableReporting: Failed to set RCB values for %s, error: %d\n", 
                   rcbRef.c_str(), error);
            LogNetworkErrorDetailed(error);
            
            // Очищаем ресурсы
            ClientReportControlBlock_destroy(rcb);
            ClientDataSet_destroy(clientDataSet);
            activeReports_.erase(rcbRef);
            
            Napi::Error::New(env, "Failed to set RCB values: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        
        printf("EnableReporting: RCB values set successfully\n");
        
        // 6. Сохраняем информацию
        reportInfo.rcb = rcb;
        reportInfo.dataSet = clientDataSet;
        
        activeReports_[rcbRef] = reportInfo;
        printf("EnableReporting: Saved ReportInfo for %s\n", rcbRef.c_str());

        // 7. Устанавливаем обработчик отчета
        printf("EnableReporting: Installing report handler for %s\n", rcbRef.c_str());
        
        // Используем BlockingCallHandler вместо стандартного обработчика
        IedConnection_installReportHandler(
            connection_,
            rcbRef.c_str(),
            ClientReportControlBlock_getRptId(rcb),
            ReportCallback,
            this
        );
        
        printf("EnableReporting: Report handler installed successfully\n");

        // 8. Отправляем уведомление
        tsfn_.BlockingCall([this, rcbRef, datasetRef, memberCount](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", "control");
                eventObj.Set("event", "reportingEnabled");
                eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
                eventObj.Set("datasetRef", Napi::String::New(env, datasetRef));
                eventObj.Set("memberCount", Napi::Number::New(env, memberCount));
                
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                printf("EnableReporting: Event sent for %s -> %s (%d members)\n", 
                       rcbRef.c_str(), datasetRef.c_str(), memberCount);
            } catch (...) {
                printf("EnableReporting: Error in callback\n");
            }
        });

        printf("EnableReporting: SUCCESS for %s -> %s, dataset has %d members\n", 
               rcbRef.c_str(), datasetRef.c_str(), memberCount);
        
        return env.Undefined();

    } catch (const std::exception& e) {
        printf("EnableReporting: Exception: %s\n", e.what());
        Napi::Error::New(env, std::string("Exception: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

Napi::Value MmsClient::DisableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();
    
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    if (isClosing_) {
        printf("DisableReporting: Client is closing, skipping for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
        return env.Undefined();
    }
    
    if (!connected_ || !connection_) {
        printf("DisableReporting: Not connected or connection invalid for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
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
            try {
                // Отключаем отчет на сервере
                ClientReportControlBlock_setRptEna(reportInfo.rcb, false);
                IedClientError error;
                IedConnection_setRCBValues(connection_, &error, reportInfo.rcb, RCB_ELEMENT_RPT_ENA, true);
                if (error != IED_ERROR_OK) {
                    printf("DisableReporting: Failed to disable RCB on server for %s, error: %d, clientID: %s\n", 
                           rcbRef.c_str(), error, clientID_.c_str());
                }
            } catch (...) {
                printf("DisableReporting: Exception while disabling RCB for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
            }
            
            ClientReportControlBlock_destroy(reportInfo.rcb);
            reportInfo.rcb = nullptr;
        }
        
        if (reportInfo.dataSet) {
            ClientDataSet_destroy(reportInfo.dataSet);
            reportInfo.dataSet = nullptr;
        }
        
        // dataSetMembers и structureElementNamesCache очистятся автоматически при удалении
        
        activeReports_.erase(it);
        
        printf("DisableReporting: Successfully disabled reporting for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
        
        // Отправляем событие
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