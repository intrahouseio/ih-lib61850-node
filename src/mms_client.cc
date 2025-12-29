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

// Структура для хранения информации о элементе структуры
struct ElementInfo {
    std::string name;
    std::string fullRef;
    FunctionalConstraint fc;
};

struct ReportInfo {
    ClientReportControlBlock rcb = nullptr;
    ClientDataSet dataSet = nullptr;
    LinkedList dataSetDirectory = nullptr;
    std::string rcbRef;
    std::string datasetRef;
    
    // Конструктор по умолчанию
    ReportInfo() = default;
    
    // Конструктор копирования
    ReportInfo(const ReportInfo& other) 
        : rcbRef(other.rcbRef), datasetRef(other.datasetRef) {
        // Указатели не копируем - они специфичны для каждого соединения
    }
    
    // Оператор присваивания
    ReportInfo& operator=(const ReportInfo& other) {
        if (this != &other) {
            rcbRef = other.rcbRef;
            datasetRef = other.datasetRef;
            // Указатели не копируем
        }
        return *this;
    }
    
    // Деструктор
    ~ReportInfo() {
        // Ресурсы освобождаются отдельно
    }
};

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
                                      
static Napi::Object ReadStructureWithServerNames(Napi::Env env, IedConnection connection, 
                                                const std::string& fullRef, MmsValue* structVal,
                                                int recursionDepth = 0);

Napi::FunctionReference MmsClient::constructor;

struct ConnectionHandlerContext {
    MmsClient* client;
    std::recursive_mutex* mutex;  // Изменено здесь
};

// Функция для получения имен элементов структуры с сервера
static std::pair<std::vector<std::string>, std::vector<MmsType>> GetStructureElementNamesFromServer(IedConnection connection,
                                  const std::string& dataRef,
                                  FunctionalConstraint fc) {
    std::vector<std::string> elementNames;
    std::vector<MmsType> elementTypes;
    
    printf("  Getting structure element names from server: %s (FC=%d)\n", 
           dataRef.c_str(), fc);
    
    IedClientError error;
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(
        connection, &error, dataRef.c_str(), fc);
    
    if (error == IED_ERROR_OK && spec != nullptr) {
        int type = MmsVariableSpecification_getType(spec);
        printf("    Variable spec type: %d\n", type);
        
        if (type == MMS_STRUCTURE) {
            int size = MmsVariableSpecification_getSize(spec);
            printf("    Structure size: %d\n", size);
            
            for (int i = 0; i < size; i++) {
                MmsVariableSpecification* childSpec = MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
                if (childSpec != nullptr) {
                    const char* name = MmsVariableSpecification_getName(childSpec);
                    MmsType childType = static_cast<MmsType>(MmsVariableSpecification_getType(childSpec));
                    
                    if (name != nullptr) {
                        elementNames.push_back(std::string(name));
                        elementTypes.push_back(childType);
                        printf("    Element [%d]: %s, type=%d\n", i, name, childType);
                    }
                }
            }
        } else {
            printf("    WARNING: Not a structure, type=%d\n", type);
        }
        
        MmsVariableSpecification_destroy(spec);
    } else {
        printf("    Failed to get variable specification, error: %d\n", error);
    }
    
    return {elementNames, elementTypes};
}

// Функция для преобразования строки FC в числовое значение
static FunctionalConstraint ParseFCFromString(const std::string& fcStr) {
    // Сначала попробуем сопоставить с текстовыми обозначениями
    std::string upperFcStr = fcStr;
    std::transform(upperFcStr.begin(), upperFcStr.end(), upperFcStr.begin(), ::toupper);
    
    printf("    ParseFCFromString: input='%s', upper='%s'\n", fcStr.c_str(), upperFcStr.c_str());

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
        printf("    [Cache-%d] Max cache depth reached for %s\n", 
               recursionDepth, baseRef.c_str());
        return;
    }
    
    printf("    [Cache-%d] START for: %s (FC=%d)\n", 
           recursionDepth, baseRef.c_str(), fc);
    
    // Получаем спецификацию
    IedClientError error;
    printf("    [Cache-%d] Calling IedConnection_getVariableSpecification...\n", recursionDepth);
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(
        connection, &error, baseRef.c_str(), fc);
    
    if (error != IED_ERROR_OK || spec == nullptr) {
        printf("    [Cache-%d] FAILED to get var spec for %s, error: %d\n", 
               recursionDepth, baseRef.c_str(), error);
        return;
    }
    
    printf("    [Cache-%d] Got var spec, type=%d\n", 
           recursionDepth, MmsVariableSpecification_getType(spec));
    
    int type = MmsVariableSpecification_getType(spec);
    
    if (type == MMS_STRUCTURE) {
        int size = MmsVariableSpecification_getSize(spec);
        printf("    [Cache-%d] Structure size: %d\n", recursionDepth, size);
        
        std::vector<std::string> elementNames;
        std::vector<MmsType> elementTypes;
        std::vector<std::pair<std::string, MmsType>> childInfo;
        
        for (int i = 0; i < size; i++) {
            printf("    [Cache-%d] Processing child %d...\n", recursionDepth, i);
            
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
                    
                    printf("    [Cache-%d]   Child %d: %s, type=%d\n", 
                           recursionDepth, i, name, childType);
                } else {
                    printf("    [Cache-%d]   Child %d: name is NULL\n", recursionDepth, i);
                }
            } else {
                printf("    [Cache-%d]   Child %d: spec is NULL\n", recursionDepth, i);
            }
        }
        
        printf("    [Cache-%d] Collected %zu children\n", recursionDepth, childInfo.size());
        
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
        printf("    [CacheStore] Storing with key: %s (FC=%d as '%s')\n", 
               refWithFc.c_str(), fc, fcStr.c_str());
        
        // УБЕДИТЕСЬ, что этот вызов выполняется (не закомментирован):
        std::lock_guard<std::recursive_mutex> lock(client->GetMutex());           
        client->CacheStructureElements(refWithFc, fc, elementNames, elementTypes);
        printf("    [Cache-%d] CacheStructureElements CALLED for %s\n", 
               recursionDepth, refWithFc.c_str());
        }
        
        printf("    [Cache-%d] Checking for recursive structures...\n", recursionDepth);
        // Рекурсивные вызовы
        for (const auto& [childName, childType] : childInfo) {
            if (childType == MMS_STRUCTURE) {
                std::string childRef = baseRef + "." + childName;
                printf("    [Cache-%d] Recursing into: %s (type=%d)\n",
                       recursionDepth, childRef.c_str(), childType);
                RecursiveCacheStructureElements(connection, client, 
                                               childRef, fc, 
                                               recursionDepth + 1);
            } else {
                printf("    [Cache-%d] Child %s is not a structure (type=%d), skipping recursion\n",
                       recursionDepth, childName.c_str(), childType);
            }
        }
        
    } else {
        printf("    [Cache-%d] Not a structure, type=%d\n", recursionDepth, type);
    }
    
    printf("    [Cache-%d] Destroying variable spec for %s...\n", recursionDepth, baseRef.c_str());
    MmsVariableSpecification_destroy(spec);
    printf("    [Cache-%d] FINISHED processing %s\n\n", recursionDepth, baseRef.c_str());
}

void MmsClient::CacheDataSetStructure(const std::string& datasetRef, 
                                     const std::vector<std::string>& memberRefs) {
    //std::lock_guard<std::mutex> lock(connMutex_);
    
    printf("!!! DEBUG: ENTERING CacheDataSetStructure !!!\n");
    printf("!!! DEBUG: datasetRef = %s\n", datasetRef.c_str());
    printf("!!! DEBUG: connected_ = %d, connection_ = %p\n", connected_, (void*)connection_);
    fflush(stdout);

    if (!connected_ || !connection_) {
        printf("CacheDataSetStructure: Not connected, skipping cache\n");
        fflush(stdout);
        return;
    }
    
    DataSetCache cache;
    cache.datasetRef = datasetRef;
    cache.memberRefs = memberRefs;
    
    printf("\n=== Recursive caching for DataSet: %s ===\n", datasetRef.c_str());
    printf("Number of members to cache: %zu\n", memberRefs.size());
    fflush(stdout);
    
    // Обрабатываем каждый элемент DataSet рекурсивно
    for (const auto& memberRef : memberRefs) {
        printf("\n  Processing member: %s\n", memberRef.c_str());
        
        // Извлекаем FC и чистую ссылку
        std::string cleanRef = memberRef;
        FunctionalConstraint fc = IEC61850_FC_ST;
        
        size_t bracketPos = memberRef.find('[');
        if (bracketPos != std::string::npos && memberRef.back() == ']') {
            std::string fcStr = memberRef.substr(bracketPos + 1, 
                                                memberRef.length() - bracketPos - 2);
            cleanRef = memberRef.substr(0, bracketPos);
            fc = ParseFCFromString(fcStr);
            printf("    Extracted: cleanRef='%s', fcStr='%s', fc=%d\n",
                   cleanRef.c_str(), fcStr.c_str(), fc);
        } else {
            printf("    WARNING: No FC in memberRef '%s', using default ST\n", memberRef.c_str());
        }
        
        // Рекурсивно кэшируем все уровни структуры
        printf("    Starting recursive cache for '%s' with FC=%d\n", cleanRef.c_str(), fc);
        RecursiveCacheStructureElements(connection_, this, cleanRef, fc, 0);
        printf("    Finished recursive cache for '%s'\n", cleanRef.c_str());
    }
    
    datasetCache_[datasetRef] = cache;
    printf("\n=== Finished recursive caching for DataSet %s ===\n", datasetRef.c_str());
    printf("Total cached datasets: %zu\n", datasetCache_.size());
}

// Исправленный метод GetCachedElementNames
bool MmsClient::GetCachedElementNames(const std::string& ref, FunctionalConstraint fc,
                                     std::vector<std::string>& elementNames) {
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    printf("  GetCachedElementNames called for ref='%s', fc=%d\n", ref.c_str(), fc);
         
    // Поиск по точному совпадению (уже есть)
    for (const auto& [datasetRef, cache] : datasetCache_) {
        auto it = cache.structureCache.find(ref);
        if (it != cache.structureCache.end()) {
            elementNames = it->second.elementNames;
            printf("  ✓ Found cached element names for EXACT ref '%s'\n", ref.c_str());
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
                printf("  Trying to find with FC: %s -> '%s'\n", fcStr.c_str(), refWithFc.c_str());
                
                for (const auto& [datasetRef, cache] : datasetCache_) {
                    auto it = cache.structureCache.find(refWithFc);
                    if (it != cache.structureCache.end()) {
                        elementNames = it->second.elementNames;
                        printf("  ✓ Found cached element names for %s (added FC %s)\n",
                               ref.c_str(), fcStr.c_str());
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
        printf("  Also trying clean ref without FC: '%s'\n", cleanRef.c_str());
        
        for (const auto& [datasetRef, cache] : datasetCache_) {
            // Ищем все ключи, которые начинаются с cleanRef
            for (const auto& [cachedRef, structInfo] : cache.structureCache) {
                if (cachedRef.find(cleanRef) == 0) {  // Начинается с cleanRef
                    elementNames = structInfo.elementNames;
                    printf("  ✓ Found cached element names via partial match: '%s' matches '%s'\n",
                           cleanRef.c_str(), cachedRef.c_str());
                    return true;
                }
            }
        }
    }
    
    printf("  ✗ No cached element names found for '%s' (fc=%d)\n", ref.c_str(), fc);
    return false;
}

// Метод для кэширования имен элементов структуры
void MmsClient::CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
                                      const std::vector<std::string>& elementNames,
                                      const std::vector<MmsType>& elementTypes) {
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    printf("  CACHE STORE: Storing structure '%s' (fc=%d) with %zu elements\n",
           ref.c_str(), fc, elementNames.size());

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
                printf("Cached structure elements for %s in DataSet %s: %zu elements\n",
                       ref.c_str(), datasetRef.c_str(), elementNames.size());
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
        
        printf("Created new cache for structure %s: %zu elements\n",
               ref.c_str(), elementNames.size());
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
    printf("    Processing structure with cache %s (size=%d, fc=%d, depth=%d)\n",
           cleanRef.c_str(), structSize, fc, recursionDepth);
    
    // Ключевое изменение: передаем cleanRef, а не fullRef
    std::vector<std::string> elementNames;
    bool hasCachedNames = false;
    
    if (client) {
        hasCachedNames = client->GetCachedElementNames(cleanRef, fc, elementNames);
    }
    
    if (hasCachedNames && elementNames.size() == static_cast<size_t>(structSize)) {
        printf("    SUCCESS: Using cached element names for %s (count: %zu)\n", 
               cleanRef.c_str(), elementNames.size());
        
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

                // Ключевое исправление: строим правильную ссылку
                //std::string elementFullRef;
                
                // Для элементов Oper внутри Mod[CO] и подобных вложенных структур
                // FC наследуется от родительской структуры
                /*if (bracketPos != std::string::npos) {
                    // У родителя есть FC - наследуем его
                    std::string fcStr = fullRef.substr(bracketPos);
                    elementFullRef = cleanRef + "." + elementName + fcStr;
                } else {
                    // У родителя нет FC - не добавляем его
                    elementFullRef = cleanRef + "." + elementName;
                }*/
                
                printf("      Element [%d]: %s (full ref: %s)\n",
                       i, elementName.c_str(), elementFullRef.c_str());
                
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
        bool isDC = (fc == IEC61850_FC_DC);
        
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
        printf("    Processing structure with cache %s (depth: %d)\n", 
               elementRef.c_str(), recursionDepth);
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
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    isClosing_ = true;
    
    if (running_) {
        running_ = false;
    }

     // Очищаем кэш
    datasetCache_.clear();
    
    // Освобождаем ресурсы отчетов
    for (auto& [rcbRef, reportInfo] : activeReports_) {
        if (reportInfo.rcb) {
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
    
    // Закрываем соединение
    if (connected_ && connection_) {
        IedConnection_close(connection_);
    }
    
    if (thread_.joinable()) {
        thread_.join();
    }
    
    if (connection_) {
        IedConnection_destroy(connection_);
    }
    
    if (tsfn_) {
        tsfn_.Release();
    }
}

void MmsClient::ConnectionHandler(void* parameter, IedConnection connection, IedConnectionState newState) {
    ConnectionHandlerContext* context = static_cast<ConnectionHandlerContext*>(parameter);
    MmsClient* client = context->client;
    ////std::mutex* mutex = context->mutex;
    std::recursive_mutex* mutex = context->mutex;

    if (client->isClosing_) {
        return;
    }

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
        ////std::lock_guard<std::mutex> lock(*mutex);  
        std::lock_guard<std::recursive_mutex> lock(*mutex);      
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
            eventObj.Set("reason", Napi::String::New(env, "connection state changed"));
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
    
    try {
        {
            ////std::lock_guard<std::mutex> lock(connMutex_);
            std::lock_guard<std::recursive_mutex> lock(connMutex_);
            isClosing_ = true;
            
            if (running_) {
                running_ = false;
                if (connected_) {
                    printf("Close called by client, clientID: %s\n", clientID_.c_str());
                    
                    tsfn_.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
                        try {
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "stateChanged"));
                            eventObj.Set("state", Napi::String::New(env, "closed"));
                            eventObj.Set("isConnected", Napi::Boolean::New(env, false));
                            eventObj.Set("reason", Napi::String::New(env, "client closed connection"));
                            jsCallback.Call({Napi::String::New(env, "conn"), eventObj});
                        } catch (const Napi::Error& e) {
                            printf("N-API Callback Error in Close: %s, clientID: %s\n", e.Message().c_str(), clientID_.c_str());
                        }
                    });
                    
                    if (connection_) {
                        IedConnection_close(connection_);
                    }
                    connected_ = false;
                }
            }
        }
        
        if (thread_.joinable()) {
            thread_.join();
        }
        
        {
            ////std::lock_guard<std::mutex> lock(connMutex_);
            std::lock_guard<std::recursive_mutex> lock(connMutex_);
            for (auto& [rcbRef, reportInfo] : activeReports_) {
                printf("Cleaning up report in Close for %s, clientID: %s\n", rcbRef.c_str(), clientID_.c_str());
                if (reportInfo.rcb) {
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
            }
            activeReports_.clear();
            
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
            int size = MmsValue_getBitStringSize(val);
            
            if (size == 2 && attrName.find("stVal") != std::string::npos) {
                uint32_t lsbValue = MmsValue_getBitStringAsInteger(val);
                uint32_t msbValue = 0;
                
                for (int i = 0; i < size; i++) {
                    int bit = (lsbValue >> i) & 1;
                    msbValue |= (bit << (size - 1 - i));
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
            else if (attrName == "q" || attrName.find(".q") != std::string::npos) {
                uint32_t bits = MmsValue_getBitStringAsInteger(val);
                data.intValue = static_cast<int64_t>(bits);
                
                // Формируем строку качества
                std::vector<std::string> qualityFlags;
                uint16_t validity = bits & 0x0003;
                
                switch (validity) {
                    case QUALITY_VALIDITY_GOOD: qualityFlags.push_back("Good"); break;
                    case QUALITY_VALIDITY_RESERVED: qualityFlags.push_back("Reserved"); break;
                    case QUALITY_VALIDITY_INVALID: qualityFlags.push_back("Invalid"); break;
                    case QUALITY_VALIDITY_QUESTIONABLE: qualityFlags.push_back("Questionable"); break;
                    default: qualityFlags.push_back("Unknown-Validity"); break;
                }
                
                // Добавляем детальные флаги
                if (bits & QUALITY_DETAIL_OVERFLOW) qualityFlags.push_back("Overflow");
                if (bits & QUALITY_DETAIL_OUT_OF_RANGE) qualityFlags.push_back("OutOfRange");
                if (bits & QUALITY_DETAIL_BAD_REFERENCE) qualityFlags.push_back("BadReference");
                if (bits & QUALITY_DETAIL_OSCILLATORY) qualityFlags.push_back("Oscillatory");
                if (bits & QUALITY_DETAIL_FAILURE) qualityFlags.push_back("Failure");
                if (bits & QUALITY_DETAIL_OLD_DATA) qualityFlags.push_back("OldData");
                if (bits & QUALITY_DETAIL_INCONSISTENT) qualityFlags.push_back("Inconsistent");
                if (bits & QUALITY_DETAIL_INACCURATE) qualityFlags.push_back("Inaccurate");
                if (bits & QUALITY_SOURCE_SUBSTITUTED) qualityFlags.push_back("Substituted");
                if (bits & QUALITY_TEST) qualityFlags.push_back("Test");
                if (bits & QUALITY_OPERATOR_BLOCKED) qualityFlags.push_back("OperatorBlocked");
                if (bits & QUALITY_DERIVED) qualityFlags.push_back("Derived");
                
                if (!qualityFlags.empty()) {
                    std::string flagsStr;
                    for (size_t i = 0; i < qualityFlags.size(); ++i) {
                        if (i > 0) flagsStr += "|";
                        flagsStr += qualityFlags[i];
                    }
                    data.stringValue = flagsStr;
                } else {
                    data.stringValue = "NoFlags";
                }
            }
            else {
                uint32_t bits = MmsValue_getBitStringAsInteger(val);
                data.intValue = static_cast<int64_t>(bits);
            }
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
            Napi::Object obj = Napi::Object::New(env);
            
            std::string name = attrName;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            
            printf("ResultDataToNapi: Processing structure attrName='%s', elements=%zu\n", 
                   attrName.c_str(), data.structureElements.size());
            
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
            
            printf("  Structure analysis: name='%s', isST=%d, isMX=%d, isDC=%d, hasPhyNam=%d, hasEEName=%d\n",
                   name.c_str(), isST, isMX, isDC, 
                   (name.find("phynam") != std::string::npos), hasEEName);
            
            if (hasSumSwARs && isST && data.structureElements.size() >= 3) {
                printf("  Recognized as SumSwARs[ST]\n");
                Napi::Object stObj = Napi::Object::New(env);
                stObj.Set("actVal", ResultDataToNapi(env, data.structureElements[0], "actVal"));
                stObj.Set("q", ResultDataToNapi(env, data.structureElements[1], "q"));
                stObj.Set("t", ResultDataToNapi(env, data.structureElements[2], "t"));
                return stObj;
            }
            
            else if (name.find("phynam") != std::string::npos && isDC) {
                printf("  Recognized as PhyNam[DC], elements=%zu\n", data.structureElements.size());
                
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
                printf("  Recognized as EEName[DC]\n");
                Napi::Object dcObj = Napi::Object::New(env);
                dcObj.Set("vendor", ResultDataToNapi(env, data.structureElements[0]));
                return dcObj;
            }
            
            else if (hasAnIn && isMX && data.structureElements.size() >= 3) {
                printf("  Recognized as AnIn[MX]\n");
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
                printf("  Recognized as FltDiskm[MX], creating named structure\n");
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
                printf("  Recognized as generic ST structure\n");
                Napi::Object stObj = Napi::Object::New(env);
                stObj.Set("stVal", ResultDataToNapi(env, data.structureElements[0], "stVal"));
                stObj.Set("q", ResultDataToNapi(env, data.structureElements[1], "q"));
                stObj.Set("t", ResultDataToNapi(env, data.structureElements[2], "t"));
                return stObj;
            }
            
            else {
                printf("  WARNING: Structure '%s' not recognized, using numeric keys (0,1,2...)\n", attrName.c_str());
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

// Функция для получения имен элементов структуры через MmsVariableSpecification
static std::vector<std::string> GetStructureElementNamesFromSpecification(IedConnection connection,
                                                                         const std::string& dataRef,
                                                                         FunctionalConstraint fc) {
    std::vector<std::string> elementNames;
    
    printf("  Getting variable specification for: %s (FC=%d)\n", dataRef.c_str(), fc);
    
    IedClientError error;
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(
        connection, &error, dataRef.c_str(), fc);
    
    if (error == IED_ERROR_OK && spec != nullptr) {
        printf("    Successfully got variable specification, type=%d\n", MmsVariableSpecification_getType(spec));
        
        // Проверяем, является ли это структурой
        if (MmsVariableSpecification_getType(spec) == MMS_STRUCTURE) {
            int size = MmsVariableSpecification_getSize(spec);
            printf("    Structure size: %d\n", size);
            
            for (int i = 0; i < size; i++) {
                MmsVariableSpecification* childSpec = MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
                if (childSpec != nullptr) {
                    const char* name = MmsVariableSpecification_getName(childSpec);
                    if (name != nullptr) {
                        std::string elementName = name;
                        elementNames.push_back(elementName);
                        printf("    Element [%d]: %s, type=%d\n", i, elementName.c_str(), 
                               MmsVariableSpecification_getType(childSpec));
                    }
                    // Освобождаем дочернюю спецификацию
                    // MmsVariableSpecification_destroy(childSpec); // Обычно не нужно, если это не копия
                }
            }
        }
        
        // Освобождаем основную спецификацию
        MmsVariableSpecification_destroy(spec);
    } else {
        printf("    Failed to get variable specification, error: %d\n", error);
    }
    
    return elementNames;
}

// Обновленная функция для чтения структуры с именами из спецификации
static Napi::Object ReadStructureWithServerNames(Napi::Env env, IedConnection connection,
                                                const std::string& fullRef, MmsValue* structVal,
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
    printf("    Processing structure %s (size=%d, fc=%d, depth=%d)\n",
           cleanRef.c_str(), structSize, fc, recursionDepth);
    
    // Получаем кэшированные имена элементов
    MmsClient* client = nullptr;
    // Нужно получить доступ к клиенту для доступа к кэшу
    // Это сложно, так как функция статическая
    
    // Вместо этого, мы будем использовать другой подход:
    // Функция ConvertMmsValueForReportWithCache будет использовать кэш через глобальный доступ
    
    std::vector<std::string> elementNames;
    bool hasCachedNames = false;
    
    // Пробуем получить имена из кэша
    // Это требует доступа к экземпляру MmsClient
    // Для простоты, мы будем использовать глобальную переменную или передавать клиента как параметр
    // Но поскольку это статическая функция, давайте изменим подход
    
    // Альтернатива: создадим новую функцию, которая будет использовать кэш
    
    return structObj;
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
            
            // Для качества (q)
            if (elementName == "q" || elementName.find(".q") != std::string::npos) {
                std::vector<std::string> qualityFlags;
                uint16_t validity = bits & 0x0003;
                
                switch (validity) {
                    case QUALITY_VALIDITY_GOOD: qualityFlags.push_back("Good"); break;
                    case QUALITY_VALIDITY_RESERVED: qualityFlags.push_back("Reserved"); break;
                    case QUALITY_VALIDITY_INVALID: qualityFlags.push_back("Invalid"); break;
                    case QUALITY_VALIDITY_QUESTIONABLE: qualityFlags.push_back("Questionable"); break;
                }
                
                if (bits & QUALITY_DETAIL_OVERFLOW) qualityFlags.push_back("Overflow");
                if (bits & QUALITY_DETAIL_OUT_OF_RANGE) qualityFlags.push_back("OutOfRange");
                if (bits & QUALITY_DETAIL_BAD_REFERENCE) qualityFlags.push_back("BadReference");
                if (bits & QUALITY_DETAIL_OSCILLATORY) qualityFlags.push_back("Oscillatory");
                if (bits & QUALITY_DETAIL_FAILURE) qualityFlags.push_back("Failure");
                if (bits & QUALITY_DETAIL_OLD_DATA) qualityFlags.push_back("OldData");
                if (bits & QUALITY_DETAIL_INCONSISTENT) qualityFlags.push_back("Inconsistent");
                if (bits & QUALITY_DETAIL_INACCURATE) qualityFlags.push_back("Inaccurate");
                if (bits & QUALITY_SOURCE_SUBSTITUTED) qualityFlags.push_back("Substituted");
                if (bits & QUALITY_TEST) qualityFlags.push_back("Test");
                if (bits & QUALITY_OPERATOR_BLOCKED) qualityFlags.push_back("OperatorBlocked");
                if (bits & QUALITY_DERIVED) qualityFlags.push_back("Derived");
                
                if (!qualityFlags.empty()) {
                    std::string flagsStr;
                    for (size_t i = 0; i < qualityFlags.size(); ++i) {
                        if (i > 0) flagsStr += "|";
                        flagsStr += qualityFlags[i];
                    }
                    return Napi::String::New(env, flagsStr);
                }
                return Napi::String::New(env, "NoFlags");
            }
            
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

Napi::Value MmsClient::ReadDataSetValues(const Napi::CallbackInfo& info) {
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

    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    Napi::Array results = Napi::Array::New(env, datasetRefs.size());

    for (size_t dsIdx = 0; dsIdx < datasetRefs.size(); ++dsIdx) {
        const std::string& datasetRef = datasetRefs[dsIdx];
        Napi::Object result = Napi::Object::New(env);
        result.Set("datasetRef", Napi::String::New(env, datasetRef));

        printf("\n=== Reading and Caching DataSet: %s ===\n", datasetRef.c_str());

        IedClientError error;
        bool isDeletable = false;

        // Получаем члены DataSet
        LinkedList members = IedConnection_getDataSetDirectory(connection_, &error, 
                                                             datasetRef.c_str(), &isDeletable);
        
        if (error != IED_ERROR_OK || !members) {
            printf("  ERROR: Cannot get dataset directory, error: %d\n", error);
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "Cannot get dataset directory"));
            results.Set(dsIdx, result);
            continue;
        }

        printf("  DataSet directory obtained successfully\n");

        // Собираем ссылки на членов
        std::vector<std::string> memberRefs;
        LinkedList entry = members;
        int memberCount = 0;
        
        while (entry) {
            if (entry->data) {
                char* memberRef = (char*)entry->data;
                memberRefs.push_back(std::string(memberRef));
                memberCount++;
                printf("  Member [%d]: %s\n", memberCount-1, memberRef);
            }
            entry = LinkedList_getNext(entry);
        }
        
        printf("  DataSet has %d members in directory\n", memberCount);

        // КЭШИРУЕМ структуры перед чтением значений
        CacheDataSetStructure(datasetRef, memberRefs);

         // Читаем значения DataSet
        ClientDataSet clientDataSet = IedConnection_readDataSetValues(connection_, &error, 
                                                                    datasetRef.c_str(), nullptr);
        if (error != IED_ERROR_OK || !clientDataSet) {
            printf("  ERROR: Cannot read dataset values, error: %d\n", error);
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "Cannot read dataset values"));
            LinkedList_destroy(members);
            results.Set(dsIdx, result);
            continue;
        }

        printf("  DataSet values read successfully\n");

        MmsValue* valuesArray = ClientDataSet_getValues(clientDataSet);
        if (!valuesArray || MmsValue_getType(valuesArray) != MMS_ARRAY) {
            printf("  ERROR: Invalid dataset values format\n");
            result.Set("isValid", false);
            result.Set("errorReason", Napi::String::New(env, "Invalid dataset values"));
            ClientDataSet_destroy(clientDataSet);
            LinkedList_destroy(members);
            results.Set(dsIdx, result);
            continue;
        }

        int arraySize = MmsValue_getArraySize(valuesArray);
        printf("  DataSet contains %d values\n", arraySize);

        Napi::Object valuesObj = Napi::Object::New(env);
        
        // Обрабатываем каждое значение        
        for (int i = 0; i < std::min(arraySize, memberCount); ++i) {
            std::string fullRef = memberRefs[i];
            printf("  Processing member [%d]: %s\n", i, fullRef.c_str());

            MmsValue* val = MmsValue_getElement(valuesArray, i);
            if (!val) {
                printf("    -> ERROR: No value at index %d\n", i);
                valuesObj.Set(fullRef, env.Null());
                continue;
            }

            int valType = MmsValue_getType(val);
            printf("    -> Value type: %d\n", valType);

            // Извлекаем имя атрибута из полной ссылки
            std::string attrName = fullRef;
            size_t lastDot = fullRef.rfind('.');
            if (lastDot != std::string::npos) {
                attrName = fullRef.substr(lastDot + 1);
            }
            
            // Используем обновленную функцию SafeConvertMmsValue, которая использует кэш
            valuesObj.Set(fullRef, SafeConvertMmsValue(env, connection_, this, fullRef, val, attrName, 0));
        }

        printf("  Processed %d values\n", std::min(arraySize, memberCount));

        result.Set("isValid", true);
        result.Set("values", valuesObj);
        result.Set("count", Napi::Number::New(env, std::min(arraySize, memberCount)));
        result.Set("isDeletable", Napi::Boolean::New(env, isDeletable));
        results.Set(dsIdx, result);

        ClientDataSet_destroy(clientDataSet);
        LinkedList_destroy(members);
        
        printf("=== Finished DataSet: %s ===\n\n", datasetRef.c_str());
    }

    tsfn_.NonBlockingCall([this, count = datasetRefs.size()](Napi::Env env, Napi::Function cb) {
        try {
            Napi::Object o = Napi::Object::New(env);
            o.Set("clientID", Napi::String::New(env, clientID_));
            o.Set("type", "data");
            o.Set("event", count == 1 ? "dataSetRead" : "multipleDataSetsRead");
            o.Set("count", static_cast<int>(count));
            cb.Call({ Napi::String::New(env, "data"), o });
        } catch (...) {}
    });

    return results;
}

Napi::Value MmsClient::BrowseDataModel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
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
                            LinkedList entry = members;                            
                            std::vector<std::string> memberRefs;

                            while (entry) {
                                if (entry->data) {
                                    char* memberRef = (char*)entry->data;
                                    std::string fullRef(memberRef);
                                    memberRefs.push_back(fullRef);                                    
                                }
                                entry = LinkedList_getNext(entry);
                            }

                            // Теперь, когда у нас есть полные имена членов из каталога сервера,
                            // мы читаем значения DataSet и сопоставляем их
                            ClientDataSet clientDataSet = IedConnection_readDataSetValues(connection_, &error, dsRef.c_str(), nullptr);
                            if (error == IED_ERROR_OK && clientDataSet) {
                                MmsValue* valuesArray = ClientDataSet_getValues(clientDataSet);
                                if (valuesArray && MmsValue_getType(valuesArray) == MMS_ARRAY) {
                                    for (size_t i = 0; i < memberRefs.size(); i++) {
                                        std::string fullRef = memberRefs[i];
                                        // fullRef содержит полную ссылку, полученную напрямую от сервера, например:
                                        // "A01LD0/LPHD1.PhyNam[DC]"
                                        // Эта строка используется как ключ и для преобразования значения
                                    }
                                }
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
            
            printf("ReadData: Parsed ref '%s' -> actualRef='%s', fc=%d\n", 
                   ref.c_str(), actualRef.c_str(), fc);
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
                    printf("ReadData: Success with alternative FC=%d for '%s'\n", 
                           fc, actualRef.c_str());
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

Napi::Value MmsClient::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    Napi::Object status = Napi::Object::New(env);
    status.Set("connected", Napi::Boolean::New(env, connected_));
    status.Set("clientID", Napi::String::New(env, clientID_.c_str()));
    return status;
}

// Новая функция для конвертации MMS значений в контексте отчетов (без запросов к серверу)
static MmsClient::ResultData ConvertMmsValueForReport(MmsValue* val, const std::string& attrName) {
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
            
            // Для stVal
            if (attrName.find("stVal") != std::string::npos && MmsValue_getBitStringSize(val) == 2) {
                uint32_t msbValue = 0;
                uint32_t lsbValue = bits;
                
                for (int i = 0; i < 2; i++) {
                    int bit = (lsbValue >> i) & 1;
                    msbValue |= (bit << (1 - i));
                }
                
                switch (msbValue) {
                    case 0: data.stringValue = "intermediate-state"; break;
                    case 1: data.stringValue = "off"; break;
                    case 2: data.stringValue = "on"; break;
                    case 3: data.stringValue = "bad-state"; break;
                }
            }
            // Для качества (q)
            else if (attrName == "q" || attrName.find(".q") != std::string::npos) {
                std::vector<std::string> qualityFlags;
                uint16_t validity = bits & 0x0003;
                
                switch (validity) {
                    case QUALITY_VALIDITY_GOOD: qualityFlags.push_back("Good"); break;
                    case QUALITY_VALIDITY_RESERVED: qualityFlags.push_back("Reserved"); break;
                    case QUALITY_VALIDITY_INVALID: qualityFlags.push_back("Invalid"); break;
                    case QUALITY_VALIDITY_QUESTIONABLE: qualityFlags.push_back("Questionable"); break;
                }
                
                if (bits & QUALITY_DETAIL_OVERFLOW) qualityFlags.push_back("Overflow");
                if (bits & QUALITY_DETAIL_OUT_OF_RANGE) qualityFlags.push_back("OutOfRange");
                if (bits & QUALITY_DETAIL_BAD_REFERENCE) qualityFlags.push_back("BadReference");
                if (bits & QUALITY_DETAIL_OSCILLATORY) qualityFlags.push_back("Oscillatory");
                if (bits & QUALITY_DETAIL_FAILURE) qualityFlags.push_back("Failure");
                if (bits & QUALITY_DETAIL_OLD_DATA) qualityFlags.push_back("OldData");
                if (bits & QUALITY_DETAIL_INCONSISTENT) qualityFlags.push_back("Inconsistent");
                if (bits & QUALITY_DETAIL_INACCURATE) qualityFlags.push_back("Inaccurate");
                if (bits & QUALITY_SOURCE_SUBSTITUTED) qualityFlags.push_back("Substituted");
                if (bits & QUALITY_TEST) qualityFlags.push_back("Test");
                if (bits & QUALITY_OPERATOR_BLOCKED) qualityFlags.push_back("OperatorBlocked");
                if (bits & QUALITY_DERIVED) qualityFlags.push_back("Derived");
                
                if (!qualityFlags.empty()) {
                    std::string flagsStr;
                    for (size_t i = 0; i < qualityFlags.size(); ++i) {
                        if (i > 0) flagsStr += "|";
                        flagsStr += qualityFlags[i];
                    }
                    data.stringValue = flagsStr;
                } else {
                    data.stringValue = "NoFlags";
                }
            }
            break;
        }

        case MMS_STRUCTURE: {
            int size = MmsValue_getArraySize(val);
            
            // Определяем тип структуры по имени
            std::string name = attrName;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            
            bool isST = false, isMX = false, isDC = false;
            
            if (name.size() >= 4) {
                std::string ending = name.substr(name.size() - 4);
                isST = (ending == "[st]");
                isMX = (ending == "[mx]");
                isDC = (ending == "[dc]");
            }
            
            // Обработка известных структур без запросов к серверу
            if (isST && size == 3) {
                // Стандартная структура статуса: stVal, q, t
                const char* stdNames[] = {"stVal", "q", "t"};
                for (int i = 0; i < size; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        std::string elementName = stdNames[i];
                        data.structureElements.push_back(ConvertMmsValueForReport(el, elementName));
                    }
                }
            } else if (isMX && size == 3) {
                // Стандартная структура измерений: mag, q, t
                const char* stdNames[] = {"mag", "q", "t"};
                for (int i = 0; i < size; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        std::string elementName = stdNames[i];
                        data.structureElements.push_back(ConvertMmsValueForReport(el, elementName));
                    }
                }
            } else if (isDC && name.find("phynam") != std::string::npos) {
                // Структура PhyNam (Device Name Plate)
                const char* dplNames[] = {"vendor", "hwRev", "swRev", "serialNum", "d", "configRev"};
                int maxNames = sizeof(dplNames) / sizeof(dplNames[0]);
                
                for (int i = 0; i < size && i < maxNames; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        std::string elementName = dplNames[i];
                        data.structureElements.push_back(ConvertMmsValueForReport(el, elementName));
                    }
                }
            } else {
                // Для неизвестных структур используем числовые индексы
                for (int i = 0; i < size; ++i) {
                    MmsValue* el = MmsValue_getElement(val, i);
                    if (el) {
                        data.structureElements.push_back(ConvertMmsValueForReport(el, std::to_string(i)));
                    }
                }
            }
            break;
        }

        case MMS_ARRAY: {
            int size = MmsValue_getArraySize(val);
            for (int i = 0; i < size; ++i) {
                MmsValue* el = MmsValue_getElement(val, i);
                if (el) {
                    data.arrayElements.push_back(ConvertMmsValueForReport(el, attrName));
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

// Функция для преобразования MMS значений в контексте отчетов с использованием кэша
static MmsClient::ResultData ConvertMmsValueForReportWithCache(MmsValue* val, 
                                                               const std::string& attrName,
                                                               const std::string& fullRef,
                                                               MmsClient* client) {
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
            
            // Для stVal
            if (attrName.find("stVal") != std::string::npos && MmsValue_getBitStringSize(val) == 2) {
                uint32_t msbValue = 0;
                uint32_t lsbValue = bits;
                
                for (int i = 0; i < 2; i++) {
                    int bit = (lsbValue >> i) & 1;
                    msbValue |= (bit << (1 - i));
                }
                
                switch (msbValue) {
                    case 0: data.stringValue = "intermediate-state"; break;
                    case 1: data.stringValue = "off"; break;
                    case 2: data.stringValue = "on"; break;
                    case 3: data.stringValue = "bad-state"; break;
                }
            }
            // Для качества (q)
            else if (attrName == "q" || attrName.find(".q") != std::string::npos) {
                std::vector<std::string> qualityFlags;
                uint16_t validity = bits & 0x0003;
                
                switch (validity) {
                    case QUALITY_VALIDITY_GOOD: qualityFlags.push_back("Good"); break;
                    case QUALITY_VALIDITY_RESERVED: qualityFlags.push_back("Reserved"); break;
                    case QUALITY_VALIDITY_INVALID: qualityFlags.push_back("Invalid"); break;
                    case QUALITY_VALIDITY_QUESTIONABLE: qualityFlags.push_back("Questionable"); break;
                }
                
                if (bits & QUALITY_DETAIL_OVERFLOW) qualityFlags.push_back("Overflow");
                if (bits & QUALITY_DETAIL_OUT_OF_RANGE) qualityFlags.push_back("OutOfRange");
                if (bits & QUALITY_DETAIL_BAD_REFERENCE) qualityFlags.push_back("BadReference");
                if (bits & QUALITY_DETAIL_OSCILLATORY) qualityFlags.push_back("Oscillatory");
                if (bits & QUALITY_DETAIL_FAILURE) qualityFlags.push_back("Failure");
                if (bits & QUALITY_DETAIL_OLD_DATA) qualityFlags.push_back("OldData");
                if (bits & QUALITY_DETAIL_INCONSISTENT) qualityFlags.push_back("Inconsistent");
                if (bits & QUALITY_DETAIL_INACCURATE) qualityFlags.push_back("Inaccurate");
                if (bits & QUALITY_SOURCE_SUBSTITUTED) qualityFlags.push_back("Substituted");
                if (bits & QUALITY_TEST) qualityFlags.push_back("Test");
                if (bits & QUALITY_OPERATOR_BLOCKED) qualityFlags.push_back("OperatorBlocked");
                if (bits & QUALITY_DERIVED) qualityFlags.push_back("Derived");
                
                if (!qualityFlags.empty()) {
                    std::string flagsStr;
                    for (size_t i = 0; i < qualityFlags.size(); ++i) {
                        if (i > 0) flagsStr += "|";
                        flagsStr += qualityFlags[i];
                    }
                    data.stringValue = flagsStr;
                } else {
                    data.stringValue = "NoFlags";
                }
            }
            break;
        }

        case MMS_STRUCTURE: {
            int size = MmsValue_getArraySize(val);
            
            // Извлекаем FC и чистую ссылку из fullRef
            std::string cleanRef = fullRef;
            FunctionalConstraint fc = IEC61850_FC_ST;
            std::string fcStr;
            
            size_t bracketPos = fullRef.find('[');
            if (bracketPos != std::string::npos && fullRef.back() == ']') {
                fcStr = fullRef.substr(bracketPos + 1, fullRef.length() - bracketPos - 2);
                cleanRef = fullRef.substr(0, bracketPos);
                fc = ParseFCFromString(fcStr);
            }
            
            // Пытаемся получить кэшированные имена
            std::vector<std::string> elementNames;
            bool hasCachedNames = false;
            
            if (client) {
                // Используем GetCachedElementNames для поиска в кэше
                hasCachedNames = client->GetCachedElementNames(cleanRef, fc, elementNames);
            }
            
            if (hasCachedNames && elementNames.size() == static_cast<size_t>(size)) {
                // ИСПОЛЬЗУЕМ КЭШИРОВАННЫЕ ИМЕНА
                printf("    [ReportCache] Using cached names for %s: %zu elements\n", 
                       cleanRef.c_str(), elementNames.size());
                
                for (int i = 0; i < size; ++i) {
                    MmsValue* childVal = MmsValue_getElement(val, i);
                    if (childVal) {
                        std::string childName = elementNames[i];
                        
                        // Строим полную ссылку для дочернего элемента
                        std::string childFullRef = cleanRef + "." + childName;
                        if (bracketPos != std::string::npos) {
                            childFullRef += "[" + fcStr + "]";
                        }
                        
                        MmsClient::ResultData childData = ConvertMmsValueForReportWithCache(childVal, childName, childFullRef, client);
                        data.structureElements.push_back(childData);
                        data.structureElementNames.push_back(childName); // Сохраняем имя
                        
                        printf("      [%d] %s -> %s\n", i, childName.c_str(), childFullRef.c_str());
                    }
                }
            } else {
                // НЕТ КЭШИРОВАННЫХ ИМЕН - используем числовые индексы
                printf("    [ReportCache] No cache for %s, using numeric indices\n", cleanRef.c_str());
                
                for (int i = 0; i < size; ++i) {
                    MmsValue* childVal = MmsValue_getElement(val, i);
                    if (childVal) {
                        std::string childName = std::to_string(i);
                        std::string childFullRef = cleanRef + "." + childName;
                        if (bracketPos != std::string::npos) {
                            childFullRef += "[" + fcStr + "]";
                        }
                        
                        MmsClient::ResultData childData = ConvertMmsValueForReportWithCache(childVal, childName, childFullRef, client);
                        data.structureElements.push_back(childData);
                        data.structureElementNames.push_back(childName);
                    }
                }
            }
            break;
        }

        case MMS_ARRAY: {
            int size = MmsValue_getArraySize(val);
            for (int i = 0; i < size; ++i) {
                MmsValue* el = MmsValue_getElement(val, i);
                if (el) {
                    std::string arrayElementName = attrName + "[" + std::to_string(i) + "]";
                    std::string arrayElementRef = fullRef + "[" + std::to_string(i) + "]";
                    data.arrayElements.push_back(
                        ConvertMmsValueForReportWithCache(el, arrayElementName, arrayElementRef, client));
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

// Функция преобразования ResultData в NAPI значение с использованием кэша
static Napi::Value ResultDataToNapiWithCache(Napi::Env env, 
                                            const MmsClient::ResultData& data, 
                                            const std::string& attrName,
                                            MmsClient* client) {
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
            printf("    [ReportToNapi] Processing structure '%s' with %zu elements\n", 
                   attrName.c_str(), data.structureElements.size());
            
            // ЕСЛИ У НАС ЕСТЬ КЭШИРОВАННЫЕ ИМЕНА - ИСПОЛЬЗУЕМ ИХ
            if (!data.structureElementNames.empty() && 
                data.structureElementNames.size() == data.structureElements.size()) {
                
                printf("      Using cached element names for structure\n");
                Napi::Object structObj = Napi::Object::New(env);
                
                for (size_t i = 0; i < data.structureElements.size(); ++i) {
                    const std::string& elementName = data.structureElementNames[i];
                    const MmsClient::ResultData& elementData = data.structureElements[i];
                    
                    printf("      Element [%zu]: %s (type=%d)\n", 
                           i, elementName.c_str(), elementData.type);
                    
                    // Для вложенных структур передаем имя элемента
                    structObj.Set(elementName,
                                ResultDataToNapiWithCache(env, elementData, elementName, client));
                }
                return structObj;
            } else {
                // НЕТ КЭШИРОВАННЫХ ИМЕН - используем числовые индексы
                printf("      No cached names, using numeric indices\n");
                Napi::Object structObj = Napi::Object::New(env);
                
                for (size_t i = 0; i < data.structureElements.size(); ++i) {
                    std::string indexName = std::to_string(i);
                    structObj.Set(indexName,
                                ResultDataToNapiWithCache(env, data.structureElements[i], indexName, client));
                }
                return structObj;
            }
        }
        
        case MMS_ARRAY: {
            Napi::Array arr = Napi::Array::New(env, data.arrayElements.size());
            for (size_t i = 0; i < data.arrayElements.size(); ++i) {
                arr.Set(i, ResultDataToNapiWithCache(env, data.arrayElements[i], attrName, client));
            }
            return arr;
        }
        
        default:
            return Napi::String::New(env, "type_" + std::to_string(data.type));
    }
}

void MmsClient::ReportCallback(void* parameter, ClientReport report) {
    MmsClient* client = static_cast<MmsClient*>(parameter);
    
    if (client->isClosing_) {
        printf("ReportCallback: Client is closing, ignoring report\n");
        return;
    }
    
    const char* rcbRefRaw = ClientReport_getRcbReference(report);
    const char* rptIdRaw = ClientReport_getRptId(report);
    std::string rcbRef = rcbRefRaw ? rcbRefRaw : "unknown";
    std::string rptId = rptIdRaw ? rptIdRaw : "unknown";

    printf("=== ReportCallback START ===\n");
    printf("Report for RCB: %s, rptId: %s, clientID: %s\n", 
           rcbRef.c_str(), rptId.c_str(), client->clientID_.c_str());

    auto it = client->activeReports_.find(rcbRef);
    if (it == client->activeReports_.end()) {
        printf("ERROR: No active report found for %s\n", rcbRef.c_str());
        printf("Active reports count: %zu\n", client->activeReports_.size());
        printf("=== ReportCallback END ===\n");
        return;
    }

    printf("Found active report for %s\n", rcbRef.c_str());
    
    LinkedList dataSetDirectory = it->second.dataSetDirectory;
    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);

    if (!dataSetValues) {
        printf("ERROR: dataSetValues is NULL\n");
        printf("=== ReportCallback END ===\n");
        return;
    }

    int dataSetType = MmsValue_getType(dataSetValues);
    int dataSetSize = MmsValue_getArraySize(dataSetValues);
    printf("dataSetValues: type=%d, array size=%d\n", dataSetType, dataSetSize);

    int dirCount = 0;
    std::vector<std::string> memberRefs;
    if (dataSetDirectory) {
        LinkedList entry = dataSetDirectory;
        while (entry) {
            if (entry->data) {
                dirCount++;
                memberRefs.push_back((char*)entry->data);
            }
            entry = LinkedList_getNext(entry);
        }
    }
    printf("DataSet directory count: %d\n", dirCount);

    if (dirCount != dataSetSize) {
        printf("WARNING: Directory count (%d) != DataSet size (%d)\n", dirCount, dataSetSize);
    }

    bool hasTimestamp = ClientReport_hasTimestamp(report);
    uint64_t timestamp = 0;
    
    if (hasTimestamp) {
        timestamp = ClientReport_getTimestamp(report);
        printf("Report has timestamp: %llu ms\n", (unsigned long long)timestamp);
    }

    printf("Processing report data with cache...\n");
    
    struct ReportItemData {
        std::string fullRef;
        std::string attrName;
        ResultData resultData;
        int reason;
    };
    
    std::vector<ReportItemData> reportItems;
    
    for (int i = 0; i < std::min(dirCount, dataSetSize); i++) {
        ReasonForInclusion reason = ClientReport_getReasonForInclusion(report, i);
        
        if (reason == IEC61850_REASON_NOT_INCLUDED) {
            continue;
        }
        
        std::string fullRef = memberRefs[i];
        
        size_t dotPos = fullRef.rfind('.');
        std::string attrName = (dotPos != std::string::npos) ? 
                               fullRef.substr(dotPos + 1) : fullRef;
        
        MmsValue* value = MmsValue_getElement(dataSetValues, i);
        if (!value) {
            printf("  [%d] %s: MmsValue is NULL\n", i, fullRef.c_str());
            continue;
        }
        
         // ИСПРАВЛЕНИЕ: Используем функцию с кэшем и передаем полную ссылку
        ResultData rd = ConvertMmsValueForReportWithCache(value, attrName, fullRef, client);
        
        ReportItemData item;
        item.fullRef = fullRef;
        item.attrName = attrName;
        item.resultData = rd;
        item.reason = reason;
        
        reportItems.push_back(item);
        
        printf("  [%d] %s: Processed with cache, type=%d, reason=%d\n", 
               i, fullRef.c_str(), rd.type, reason);
    }

    printf("Sending report event to JS...\n");
    
    client->tsfn_.NonBlockingCall([client, rcbRef, rptId, timestamp, hasTimestamp, reportItems](Napi::Env env, Napi::Function cb) {
        try {
            printf("Processing report in JS thread for RCB: %s\n", rcbRef.c_str());
            
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
            
            for (const auto& item : reportItems) {
                // Используем специальную функцию для преобразования с кэшем
                Napi::Value jsValue = ResultDataToNapiWithCache(env, item.resultData, item.attrName, client);
                valuesObj.Set(item.fullRef, jsValue);
                reasonsObj.Set(item.fullRef, item.reason);
            }
            
            eventObj.Set("values", valuesObj);
            eventObj.Set("reasons", reasonsObj);

            cb.Call({Napi::String::New(env, "data"), eventObj});
            printf("Report event sent to JS successfully for RCB: %s\n", rcbRef.c_str());
        } catch (const Napi::Error& e) {
            printf("ERROR in NonBlockingCall: %s\n", e.Message().c_str());
        } catch (const std::exception& e) {
            printf("ERROR in NonBlockingCall (std): %s\n", e.what());
        } catch (...) {
            printf("ERROR in NonBlockingCall: unknown exception\n");
        }
    });
    
    printf("=== ReportCallback END ===\n");
}

Napi::Value MmsClient::EnableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string) and datasetRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();
    std::string datasetRef = info[1].As<Napi::String>().Utf8Value();
    
    ////std::lock_guard<std::mutex> lock(connMutex_);
    std::lock_guard<std::recursive_mutex> lock(connMutex_);
    
    if (!connected_ || !connection_) {
        printf("EnableReporting: Not connected\n");
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
        LinkedList dataSetDirectory = IedConnection_getDataSetDirectory(
            connection_, &error, datasetRef.c_str(), &isDeletable);
        
        if (error != IED_ERROR_OK || !dataSetDirectory) {
            printf("EnableReporting: Failed to get dataset directory for %s, error: %d\n", 
                   datasetRef.c_str(), error);
            Napi::Error::New(env, "Failed to get dataset directory: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        printf("EnableReporting: Got dataset directory for %s\n", datasetRef.c_str());

        // 2. Читаем DataSet
        ClientDataSet clientDataSet = IedConnection_readDataSetValues(
            connection_, &error, datasetRef.c_str(), nullptr);
        
        if (error != IED_ERROR_OK || !clientDataSet) {
            printf("EnableReporting: Failed to read dataset %s, error: %d\n", 
                   datasetRef.c_str(), error);
            LinkedList_destroy(dataSetDirectory);
            Napi::Error::New(env, "Failed to read dataset: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        printf("EnableReporting: Read dataset for %s\n", datasetRef.c_str());

        // 3. Получаем RCB
        ClientReportControlBlock rcb = IedConnection_getRCBValues(
            connection_, &error, rcbRef.c_str(), nullptr);
        
        if (error != IED_ERROR_OK || !rcb) {
            printf("EnableReporting: Failed to get RCB %s, error: %d\n", 
                   rcbRef.c_str(), error);
            ClientDataSet_destroy(clientDataSet);
            LinkedList_destroy(dataSetDirectory);
            Napi::Error::New(env, "Failed to get RCB: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        printf("EnableReporting: Got RCB for %s, rptId: %s\n", 
               rcbRef.c_str(), ClientReportControlBlock_getRptId(rcb));

        // 4. Настраиваем RCB (ТОЧНО как в mms_client2.cc!)
        ClientReportControlBlock_setResv(rcb, true);
        ClientReportControlBlock_setTrgOps(rcb, 
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_INTEGRITY | TRG_OPT_GI);
        ClientReportControlBlock_setDataSetReference(rcb, datasetRef.c_str());
        ClientReportControlBlock_setRptEna(rcb, true);
        ClientReportControlBlock_setIntgPd(rcb, 3000);
        ClientReportControlBlock_setGI(rcb, true);

        // 5. Устанавливаем обработчик
        IedConnection_installReportHandler(
            connection_,
            rcbRef.c_str(),
            ClientReportControlBlock_getRptId(rcb),
            ReportCallback,
            this
        );
        printf("EnableReporting: Installed report handler for %s\n", rcbRef.c_str());

        // 6. Применяем настройки RCB (ВАЖНО: маска как в mms_client2.cc!)
        // НЕ включаем RCB_ELEMENT_RESV и RCB_ELEMENT_DATSET в маску!
        IedConnection_setRCBValues(connection_, &error, rcb, 
            RCB_ELEMENT_TRG_OPS | RCB_ELEMENT_RPT_ENA | 
            RCB_ELEMENT_GI | RCB_ELEMENT_INTG_PD, true);

        if (error != IED_ERROR_OK) {
            printf("EnableReporting: Failed to set RCB values for %s, error: %d\n", 
                   rcbRef.c_str(), error);
            
            // Удаляем обработчик
            IedConnection_installReportHandler(connection_, rcbRef.c_str(), 
                ClientReportControlBlock_getRptId(rcb), nullptr, nullptr);
            
            ClientReportControlBlock_destroy(rcb);
            ClientDataSet_destroy(clientDataSet);
            LinkedList_destroy(dataSetDirectory);
            
            Napi::Error::New(env, "Failed to set RCB values: " + std::to_string(error)).ThrowAsJavaScriptException();
            return env.Undefined();
        }

        // 7. Сохраняем информацию
        ReportInfo reportInfo;
        reportInfo.rcb = rcb;
        reportInfo.dataSet = clientDataSet;
        reportInfo.dataSetDirectory = dataSetDirectory;
        reportInfo.rcbRef = rcbRef;
        reportInfo.datasetRef = datasetRef;

        // ВАЖНО: Сохраняем также имена элементов
        std::vector<std::string> memberNames;
        LinkedList entry = dataSetDirectory;
        while (entry) {
            if (entry->data) {
                memberNames.push_back((char*)entry->data);
            }
            entry = LinkedList_getNext(entry);
        }
        // Можно сохранить memberNames в ReportInfo, если нужно
        
        activeReports_[rcbRef] = reportInfo;
        printf("EnableReporting: Saved ReportInfo for %s, active reports: %zu\n", 
               rcbRef.c_str(), activeReports_.size());

        // 8. Отправляем уведомление
        tsfn_.NonBlockingCall([this, rcbRef](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, clientID_.c_str()));
                eventObj.Set("type", "control");
                eventObj.Set("event", "reportingEnabled");
                eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
                jsCallback.Call({Napi::String::New(env, "data"), eventObj});
                printf("EnableReporting: Event sent for %s\n", rcbRef.c_str());
            } catch (...) {
                printf("EnableReporting: Error in callback\n");
            }
        });

        printf("EnableReporting: SUCCESS for %s -> %s\n", rcbRef.c_str(), datasetRef.c_str());
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
    
    ////std::lock_guard<std::mutex> lock(connMutex_);
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
                ClientReportControlBlock_setRptEna(reportInfo.rcb, false);
                IedClientError error;
                IedConnection_setRCBValues(connection_, &error, reportInfo.rcb, RCB_ELEMENT_RPT_ENA, true);
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