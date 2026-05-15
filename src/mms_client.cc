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
static void RecursiveCacheStructureElements(IedConnection connection,
                                          MmsClient* client,
                                          const std::string& baseRef,
                                          FunctionalConstraint fc,
                                          int recursionDepth);

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



static std::string FunctionalConstraintToString(FunctionalConstraint fc) {
    switch (fc) {
        case IEC61850_FC_ST: return "ST";
        case IEC61850_FC_MX: return "MX";
        case IEC61850_FC_CO: return "CO";
        case IEC61850_FC_CF: return "CF";
        case IEC61850_FC_DC: return "DC";
        case IEC61850_FC_SP: return "SP";
        case IEC61850_FC_SG: return "SG";
        case IEC61850_FC_BR: return "BR";
        case IEC61850_FC_RP: return "RP";
        case IEC61850_FC_EX: return "EX";
        case IEC61850_FC_SR: return "SR";
        case IEC61850_FC_OR: return "OR";
        case IEC61850_FC_BL: return "BL";
        case IEC61850_FC_LG: return "LG";
        case IEC61850_FC_GO: return "GO";
        case IEC61850_FC_MS: return "MS";
        case IEC61850_FC_US: return "US";
        case IEC61850_FC_ALL: return "ALL";
        default: return std::to_string(fc);
    }
}



namespace {
    // Преобразование MmsType в строку
    static std::string MmsTypeToString(MmsType type) {
        switch (type) {
            case MMS_ARRAY:           return "Array";
            case MMS_BOOLEAN:         return "Boolean";
            case MMS_BIT_STRING:      return "BitString";
            case MMS_FLOAT:           return "Float";
            case MMS_INTEGER:         return "Integer";
            case MMS_UNSIGNED:        return "Unsigned";
            case MMS_STRUCTURE:       return "Structure";
            case MMS_VISIBLE_STRING:  return "VisibleString";
            case MMS_UTC_TIME:        return "UtcTime";
            case MMS_OCTET_STRING:    return "OctetString";
            default:                  return "Unknown";
        }
    }

    // Определение CDC по ссылке DataObject (через спецификацию stVal или mag)
    static std::string GetCdcForDataObject(IedConnection connection, const std::string& doRef) {
        IedClientError error;
        // Пробуем прочитать stVal (для DPC, SPS, DPS)
        std::string stValRef = doRef + ".stVal";
        MmsVariableSpecification* spec = IedConnection_getVariableSpecification(connection, &error, stValRef.c_str(), IEC61850_FC_ST);
        if (error == IED_ERROR_OK && spec) {
            MmsType type = static_cast<MmsType>(MmsVariableSpecification_getType(spec));
            MmsVariableSpecification_destroy(spec);
            if (type == MMS_BIT_STRING && MmsVariableSpecification_getSize(spec) == 2) return "DPC";
            if (type == MMS_BIT_STRING && MmsVariableSpecification_getSize(spec) == 1) return "SPS";
            if (type == MMS_INTEGER) return "INS";
            if (type == MMS_STRUCTURE) return "ACT";
        }
        // Пробуем mag (для MV)
        std::string magRef = doRef + ".mag";
        spec = IedConnection_getVariableSpecification(connection, &error, magRef.c_str(), IEC61850_FC_MX);
        if (error == IED_ERROR_OK && spec) {
            MmsType type = static_cast<MmsType>(MmsVariableSpecification_getType(spec));
            MmsVariableSpecification_destroy(spec);
            if (type == MMS_STRUCTURE) return "MV";
        }
        // Пробуем namPlt (LPL)
        std::string namPltRef = doRef + ".namPlt";
        spec = IedConnection_getVariableSpecification(connection, &error, namPltRef.c_str(), IEC61850_FC_DC);
        if (error == IED_ERROR_OK && spec) {
            MmsVariableSpecification_destroy(spec);
            return "LPL";
        }
        // По умолчанию – неизвестно
        return "Unknown";
    }

    // Структуры для хранения результатов обхода модели (используются в BrowseDataModelWorker)

    struct DataAttributeInfo {
        std::string name;
        std::string reference;
        std::string type;               // "dataAttribute"
        std::string mmsType;            // строковое представление MMS типа
        std::string functionalConstraint;
    };

    struct DataObjectDetails {
        std::string reference;
        std::string name;
        std::vector<DataAttributeInfo> attributes;
    };

    struct DataSetInfo {
        std::string name;
        std::string reference;
        std::string type; // "dataset"
    };

    struct ReportInfo {
        std::string name;
        std::string reference;
        std::string type; // "RP" или "BR"
        std::string description;
    };

    struct DataObjectInfo {
        std::string name;
        std::string reference;
        std::string cdc; // тип CDC (DPC, SPS, MV и т.д.)
    };

    struct LogicalNodeInfo {
        std::string name;
        std::string reference;
        std::vector<DataObjectInfo> dataObjects;
        std::vector<DataSetInfo> dataSets;
        std::vector<ReportInfo> reports;
    };

    // Детали для логического узла (используются при передаче конкретной ссылки)
    struct LogicalNodeDetails {
        std::string reference;
        std::string name;
        std::vector<DataObjectInfo> dataObjects;
        std::vector<DataSetInfo> dataSets;
        int dataObjectsCount;
        int dataSetsCount;
    }; 

    struct DataSetDetails {
        std::string reference;
        std::string name;
        bool isDeletable;
        bool isValid;
        std::string errorReason;
        bool alreadyCached;
        int memberCount;
        std::vector<std::pair<std::string, std::string>> members; // <reference, name>
    };

    struct ReportDetails {
        std::string reference;
        std::string name;
        std::string reportType;
        std::string datasetRef;
        std::string originalDatasetRef;
        bool enabled;
        std::string reportId;
        int trgOps;
        int intgPd;
        int bufTm;
        bool gi;
        bool isValid;
        bool datasetAlreadyCached;
        std::string errorReason;
    };

    // Результат обхода – объединение возможных типов
    struct BrowseResult {
        enum Type { ROOT_NODES, LOGICAL_NODE, DATA_OBJECT, DATA_SET, REPORT, ERROR };
        Type type;
        std::string errorReason;
        std::vector<LogicalNodeInfo> rootNodes;
        LogicalNodeDetails logicalNode;
        DataObjectDetails dataObject;
        DataSetDetails dataSet;
        ReportDetails report;
    };

    //вспомогательную функцию для определения FC по имени атрибута
    static std::string GetFunctionalConstraintForAttribute(const std::string& attrName) {
        // CF – configuration
        if (attrName == "ctlModel" || attrName == "cfgModel" || attrName == "d" || attrName == "dU" ||
            attrName == "dataNs" || attrName == "operTimeout" || attrName == "blkEna" || attrName == "numCtl" ||
            attrName == "min" || attrName == "max" || attrName == "step" || attrName == "unit")
            return "CF";
        // CO – control
        if (attrName == "Oper" || attrName == "SBO" || attrName == "SBOw" || attrName == "Cancel")
            return "CO";
        // ST – status
        if (attrName == "stVal" || attrName == "q" || attrName == "t")
            return "ST";
        // MX – measurement
        if (attrName == "mag" || attrName == "range" || attrName == "db" || 
            attrName == "zeroDb" || attrName == "sVC")
            return "MX";
        // DC – description
        if (attrName == "namPlt" || attrName == "phyNam" || attrName == "configRev" || 
            attrName == "ldNs" || attrName == "lnNs")
            return "DC";
        return "unknown";
    }

    // Получить список логических устройств и узлов (только имена, без подробностей)
    static std::vector<LogicalNodeInfo> GetRootNodesWorker(IedConnection connection) {
        std::vector<LogicalNodeInfo> result;
        IedClientError error;
        LinkedList deviceList = IedConnection_getLogicalDeviceList(connection, &error);
        if (error != IED_ERROR_OK || !deviceList) return result;

        LinkedList device = deviceList;
        while (device) {
            char* ldName = (char*)device->data;
            if (ldName) {
                LinkedList nodeList = IedConnection_getLogicalDeviceDirectory(connection, &error, ldName);
                if (error == IED_ERROR_OK && nodeList) {
                    LinkedList node = nodeList;
                    while (node) {
                        char* lnName = (char*)node->data;
                        if (lnName) {
                            std::string lnRef = std::string(ldName) + "/" + lnName;
                            LogicalNodeInfo lnInfo;
                            lnInfo.name = lnName;
                            lnInfo.reference = lnRef;

                            // Получаем DataSets (без мьютекса)
                            LinkedList dataSetList = IedConnection_getLogicalNodeDirectory(connection, &error, lnRef.c_str(), ACSI_CLASS_DATA_SET);
                            if (error == IED_ERROR_OK && dataSetList) {
                                LinkedList ds = dataSetList;
                                while (ds) {
                                    char* dsName = (char*)ds->data;
                                    if (dsName) {
                                        DataSetInfo dsi;
                                        dsi.name = dsName;
                                        dsi.reference = lnRef + "." + dsName;
                                        dsi.type = "dataset";
                                        lnInfo.dataSets.push_back(dsi);
                                    }
                                    ds = LinkedList_getNext(ds);
                                }
                                LinkedList_destroy(dataSetList);
                            }

                            // Получаем Reports (без мьютекса)
                            LinkedList varList = IedConnection_getLogicalNodeVariables(connection, &error, lnRef.c_str());
                            if (error == IED_ERROR_OK && varList) {
                                LinkedList var = varList;
                                while (var) {
                                    char* varName = (char*)var->data;
                                    if (varName) {
                                        std::string varStr(varName);
                                        if ((varStr.find("RP$") == 0 || varStr.find("BR$") == 0) &&
                                            std::count(varStr.begin(), varStr.end(), '$') == 1) {
                                            ReportInfo rpt;
                                            rpt.name = varName;
                                            rpt.reference = lnRef + "." + varName;
                                            rpt.type = (varStr.find("RP$") == 0) ? "RP" : "BR";
                                            rpt.description = (rpt.type == "RP") ? "Unbuffered Report" : "Buffered Report";
                                            lnInfo.reports.push_back(rpt);
                                        }
                                    }
                                    var = LinkedList_getNext(var);
                                }
                                LinkedList_destroy(varList);
                            }

                            result.push_back(lnInfo);
                        }
                        node = LinkedList_getNext(node);
                    }
                    LinkedList_destroy(nodeList);
                }
            }
            device = LinkedList_getNext(device);
        }
        LinkedList_destroy(deviceList);
        return result;
    }

    // Получить детали логического узла
    /*static LogicalNodeDetails GetLogicalNodeDetailsWorker(IedConnection connection, const std::string& lnRef) {
        LogicalNodeDetails details;
        details.reference = lnRef;
        size_t slashPos = lnRef.find_last_of('/');
        if (slashPos != std::string::npos)
            details.name = lnRef.substr(slashPos + 1);
        else
            details.name = lnRef;

        IedClientError error;

        // DataObjects – без мьютекса
        LinkedList doList = IedConnection_getLogicalNodeDirectory(connection, &error, lnRef.c_str(), ACSI_CLASS_DATA_OBJECT);
        if (error == IED_ERROR_OK && doList) {
            LinkedList doItem = doList;
            while (doItem) {
                char* doName = (char*)doItem->data;
                if (doName) {
                    std::string doNameStr(doName);
                    if (doNameStr.find("RP$") == 0 || doNameStr.find("BR$") == 0) {
                        doItem = LinkedList_getNext(doItem);
                        continue;
                    }
                    DataObjectInfo doi;
                    doi.name = doName;
                    doi.reference = lnRef + "." + doName;
                    // Определение CDC (без изменений)
                    std::string lower = doNameStr;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find("pos") != std::string::npos || lower.find("swi") != std::string::npos)
                        doi.cdc = "DPC";
                    else if (lower.find("ind") != std::string::npos || lower.find("alm") != std::string::npos || lower.find("tr") != std::string::npos)
                        doi.cdc = "SPS";
                    else if (lower.find("anin") != std::string::npos || lower.find("mag") != std::string::npos)
                        doi.cdc = "MV";
                    else if (lower.find("mod") != std::string::npos)
                        doi.cdc = "INC";
                    else if (lower.find("beh") != std::string::npos)
                        doi.cdc = "INS";
                    else if (lower.find("max") != std::string::npos || lower.find("set") != std::string::npos)
                        doi.cdc = "ASG";
                    else if (lower.find("phy") != std::string::npos || lower.find("nam") != std::string::npos)
                        doi.cdc = "LPL";
                    else if (lower.find("str") != std::string::npos)
                        doi.cdc = "DPL";
                    else
                        doi.cdc = "Unknown";
                    details.dataObjects.push_back(doi);
                }
                doItem = LinkedList_getNext(doItem);
            }
            LinkedList_destroy(doList);
        }

        // DataSets
        LinkedList dsList = IedConnection_getLogicalNodeDirectory(connection, &error, lnRef.c_str(), ACSI_CLASS_DATA_SET);
        if (error == IED_ERROR_OK && dsList) {
            LinkedList dsItem = dsList;
            while (dsItem) {
                char* dsName = (char*)dsItem->data;
                if (dsName) {
                    DataSetInfo dsi;
                    dsi.name = dsName;
                    dsi.reference = lnRef + "." + dsName;
                    dsi.type = "dataset";
                    details.dataSets.push_back(dsi);
                }
                dsItem = LinkedList_getNext(dsItem);
            }
            LinkedList_destroy(dsList);
        }

        details.dataObjectsCount = details.dataObjects.size();
        details.dataSetsCount = details.dataSets.size();
        return details;
    }*/

    static LogicalNodeDetails GetLogicalNodeDetailsWorker(IedConnection connection, const std::string& lnRef) {
        LogicalNodeDetails details;
        details.reference = lnRef;
        size_t slashPos = lnRef.find_last_of('/');
        if (slashPos != std::string::npos)
            details.name = lnRef.substr(slashPos + 1);
        else
            details.name = lnRef;

        IedClientError error;

        // DataObjects – получаем список
        LinkedList doList = IedConnection_getLogicalNodeDirectory(connection, &error, lnRef.c_str(), ACSI_CLASS_DATA_OBJECT);
        if (error == IED_ERROR_OK && doList) {
            LinkedList doItem = doList;
            while (doItem) {
                char* doName = (char*)doItem->data;
                if (doName) {
                    std::string doNameStr(doName);
                    // Пропускаем элементы, которые не являются DataObjects (Report Control Blocks)
                    if (doNameStr.find("RP$") == 0 || doNameStr.find("BR$") == 0) {
                        doItem = LinkedList_getNext(doItem);
                        continue;
                    }
                    DataObjectInfo doi;
                    doi.name = doName;
                    std::string doRef = lnRef + "." + doName;
                    doi.reference = doRef;
                    
                    // *** ТОЧНОЕ ОПРЕДЕЛЕНИЕ CDC через чтение с сервера ***
                    doi.cdc = GetCdcForDataObject(connection, doRef);
                    
                    details.dataObjects.push_back(doi);
                }
                doItem = LinkedList_getNext(doItem);
            }
            LinkedList_destroy(doList);
        }

        // DataSets (без изменений)
        LinkedList dsList = IedConnection_getLogicalNodeDirectory(connection, &error, lnRef.c_str(), ACSI_CLASS_DATA_SET);
        if (error == IED_ERROR_OK && dsList) {
            LinkedList dsItem = dsList;
            while (dsItem) {
                char* dsName = (char*)dsItem->data;
                if (dsName) {
                    DataSetInfo dsi;
                    dsi.name = dsName;
                    dsi.reference = lnRef + "." + dsName;
                    dsi.type = "dataset";
                    details.dataSets.push_back(dsi);
                }
                dsItem = LinkedList_getNext(dsItem);
            }
            LinkedList_destroy(dsList);
        }

        details.dataObjectsCount = details.dataObjects.size();
        details.dataSetsCount = details.dataSets.size();
        return details;
    }

    static DataSetDetails GetDataSetDetailsWorker(IedConnection connection, MmsClient* client, const std::string& dsRef) {
    DataSetDetails details;
    details.reference = dsRef;
    size_t dotPos = dsRef.find_last_of('.');
    if (dotPos != std::string::npos)
        details.name = dsRef.substr(dotPos + 1);
    else
        details.name = dsRef;

    details.isValid = true;

    // Проверка кэша – короткая блокировка
    bool cached = false;
    {
        std::lock_guard<std::recursive_timed_mutex> lock(client->GetMutex());
        auto it = client->GetDataSetCache().find(dsRef);
        if (it != client->GetDataSetCache().end()) {
            cached = true;
            details.alreadyCached = true;
            details.isDeletable = false;
            details.memberCount = it->second.memberRefs.size();
            for (const auto& memberRef : it->second.memberRefs) {
                std::string memberName = memberRef;
                size_t lastDot = memberRef.rfind('.');
                if (lastDot != std::string::npos)
                    memberName = memberRef.substr(lastDot + 1);
                details.members.emplace_back(memberRef, memberName);
            }
        }
    }
    if (cached) return details;

    details.alreadyCached = false;
    IedClientError error;
    bool isDeletable = false;
    LinkedList members = IedConnection_getDataSetDirectory(connection, &error, dsRef.c_str(), &isDeletable);
    if (error != IED_ERROR_OK || !members) {
        details.isValid = false;
        details.errorReason = "Cannot get dataset directory, error: " + std::to_string(error);
        return details;
    }

    details.isDeletable = isDeletable;
    std::vector<std::string> memberRefs;
    LinkedList entry = members;
    while (entry) {
        if (entry->data) {
            char* memberRef = (char*)entry->data;
            memberRefs.push_back(std::string(memberRef));
            std::string memberName = std::string(memberRef);
            size_t lastDot = memberRefs.back().rfind('.');
            if (lastDot != std::string::npos)
                memberName = memberRefs.back().substr(lastDot + 1);
            details.members.emplace_back(std::string(memberRef), memberName);
        }
        entry = LinkedList_getNext(entry);
    }
    LinkedList_destroy(members);
    details.memberCount = memberRefs.size();

    // Кэшируем – короткая блокировка
    {
        std::lock_guard<std::recursive_timed_mutex> lock(client->GetMutex());
        client->CacheDataSetStructure(dsRef, memberRefs);
    }

    return details;
}

   
    static void CollectStructureInfo(IedConnection connection,
                                    const std::string& baseRef,
                                    FunctionalConstraint fc,
                                    std::unordered_map<std::string, StructureElementNames>& outCache,
                                    int recursionDepth = 0) {
        const int MAX_DEPTH = 5;
        if (recursionDepth > MAX_DEPTH) return;

        IedClientError error;
        MmsVariableSpecification* spec = IedConnection_getVariableSpecification(connection, &error, baseRef.c_str(), fc);
        
        // Если не удалось получить спецификацию с заданным FC, пробуем другие
        if (error != IED_ERROR_OK || !spec) {
            // Пробуем ST, MX, CF, ALL
            std::vector<FunctionalConstraint> fallbackFc = {
                IEC61850_FC_ST, IEC61850_FC_MX, IEC61850_FC_CF,
                IEC61850_FC_DC, IEC61850_FC_SP, IEC61850_FC_SG,
                IEC61850_FC_ALL
            };
            for (auto tryFc : fallbackFc) {
                if (tryFc == fc) continue;
                spec = IedConnection_getVariableSpecification(connection, &error, baseRef.c_str(), tryFc);
                if (error == IED_ERROR_OK && spec) {
                    fc = tryFc; // используем найденный FC
                    break;
                }
            }
        }
        
        if (error != IED_ERROR_OK || !spec) {
            // Если всё равно не получили, возможно, это структура, которую нужно обработать иначе
            // Например, для Control Object можно попробовать получить спецификацию для baseRef + ".Oper"
            std::string operRef = baseRef + ".Oper";
            spec = IedConnection_getVariableSpecification(connection, &error, operRef.c_str(), IEC61850_FC_CO);
            if (error != IED_ERROR_OK || !spec) {
                return; // совсем не удалось
            }
            // Успешно получили спецификацию для Oper, но тогда имена будут для Oper, а не для Mod
            // Поэтому нужно скорректировать baseRef? Это сложно. Проще сохранить кэш для Mod как есть,
            // но с именами от Oper? Неправильно.
            // Лучше вернуться и не кэшировать.
            MmsVariableSpecification_destroy(spec);
            return;
        }

        int type = MmsVariableSpecification_getType(spec);
        if (type == MMS_STRUCTURE) {
            int size = MmsVariableSpecification_getSize(spec);
            std::vector<std::string> names;
            std::vector<MmsType> types;
            for (int i = 0; i < size; ++i) {
                MmsVariableSpecification* child = MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
                if (child) {
                    const char* name = MmsVariableSpecification_getName(child);
                    if (name) {
                        names.push_back(name);
                        types.push_back(static_cast<MmsType>(MmsVariableSpecification_getType(child)));
                        if (types.back() == MMS_STRUCTURE) {
                            std::string childRef = baseRef + "." + name;
                            CollectStructureInfo(connection, childRef, fc, outCache, recursionDepth + 1);
                        }
                    }
                }
            }
            std::string fcStr = FunctionalConstraint_toString(fc);
            std::string cacheKey = baseRef + "[" + fcStr + "]";
            outCache[cacheKey] = {cacheKey, fc, names, types};
            printf("CollectStructureInfo: Cached %s with %zu names\n", cacheKey.c_str(), names.size());
        }
        MmsVariableSpecification_destroy(spec);
    }

    static ReportDetails GetReportDetailsWorker(IedConnection connection, MmsClient* client, const std::string& rRef) {
    ReportDetails details;
    details.reference = rRef;
    size_t dotPos = rRef.find_last_of('.');
    if (dotPos != std::string::npos)
        details.name = rRef.substr(dotPos + 1);
    else
        details.name = rRef;

    if (rRef.find("RP$") != std::string::npos)
        details.reportType = "RP";
    else if (rRef.find("BR$") != std::string::npos)
        details.reportType = "BR";
    else {
        details.isValid = false;
        details.errorReason = "Unknown report type";
        return details;
    }

    IedClientError error;
    ClientReportControlBlock rcb = IedConnection_getRCBValues(connection, &error, rRef.c_str(), nullptr);
    if (error != IED_ERROR_OK || !rcb) {
        details.isValid = false;
        details.errorReason = "Cannot get RCB values, error: " + std::to_string(error);
        return details;
    }

    const char* datasetRefRaw = ClientReportControlBlock_getDataSetReference(rcb);
    if (datasetRefRaw) {
        details.datasetRef = datasetRefRaw;
        details.originalDatasetRef = datasetRefRaw;
        std::string normalized = details.datasetRef;
        std::replace(normalized.begin(), normalized.end(), '$', '.');
        details.datasetRef = normalized;

        // Проверка кэша – короткая блокировка
        bool cached = false;
        {
            std::lock_guard<std::recursive_timed_mutex> lock(client->GetMutex());
            cached = client->GetDataSetCache().find(normalized) != client->GetDataSetCache().end();
        }
        details.datasetAlreadyCached = cached;
        if (!cached) {
            bool isDeletable = false;
            LinkedList members = IedConnection_getDataSetDirectory(connection, &error, normalized.c_str(), &isDeletable);
            if (error == IED_ERROR_OK && members) {
                std::vector<std::string> memberRefs;
                LinkedList entry = members;
                while (entry) {
                    if (entry->data)
                        memberRefs.push_back((char*)entry->data);
                    entry = LinkedList_getNext(entry);
                }
                LinkedList_destroy(members);
                {
                    std::lock_guard<std::recursive_timed_mutex> lock(client->GetMutex());
                    client->CacheDataSetStructure(normalized, memberRefs);
                }
            }
        }
    }

    details.enabled = ClientReportControlBlock_getRptEna(rcb);
    const char* rptId = ClientReportControlBlock_getRptId(rcb);
    if (rptId) details.reportId = rptId;
    details.trgOps = ClientReportControlBlock_getTrgOps(rcb);
    details.intgPd = ClientReportControlBlock_getIntgPd(rcb);
    details.bufTm = ClientReportControlBlock_getBufTm(rcb);
    details.gi = ClientReportControlBlock_getGI(rcb);

    ClientReportControlBlock_destroy(rcb);
    details.isValid = true;
    return details;
}

    class BrowseDataModelWorker : public Napi::AsyncWorker {
    public:
        BrowseDataModelWorker(MmsClient* client,
                            IedConnection connection,
                            std::recursive_timed_mutex& connMutex,
                            Napi::Env env,
                            const std::string& ref,
                            Napi::Promise::Deferred deferred)
            : Napi::AsyncWorker(env),
            client_(client),
            connection_(connection),
            connMutex_(connMutex),
            env_(env),
            ref_(ref),
            deferred_(deferred) {}

        ~BrowseDataModelWorker() {}

        void Execute() override {
            IedConnection localConn = nullptr;
            bool isConnected = false;
            {
                std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                isConnected = client_->IsConnected();
                localConn = client_->GetConnection();
            }
            if (!isConnected || !localConn) {
                result_.type = BrowseResult::ERROR;
                result_.errorReason = "Not connected";
                return;
            }

            if (ref_.empty()) {
                result_.type = BrowseResult::ROOT_NODES;
                result_.rootNodes = GetRootNodesWorker(localConn);
            } else {
                size_t dotPos = ref_.find('.');
                if (dotPos == std::string::npos) {
                    result_.type = BrowseResult::LOGICAL_NODE;
                    result_.logicalNode = GetLogicalNodeDetailsWorker(localConn, ref_);
                } else {
                    std::string objectPart = ref_.substr(dotPos + 1);
                    IedClientError error;
                    bool isDeletable = false;
                    LinkedList members = IedConnection_getDataSetDirectory(localConn, &error, ref_.c_str(), &isDeletable);
                    if (error == IED_ERROR_OK && members) {
                        LinkedList_destroy(members);
                        result_.type = BrowseResult::DATA_SET;
                        result_.dataSet = GetDataSetDetailsWorker(localConn, client_, ref_);
                    } else if (objectPart.find('$') != std::string::npos) {
                        result_.type = BrowseResult::REPORT;
                        result_.report = GetReportDetailsWorker(localConn, client_, ref_);
                    } else {
                        result_.type = BrowseResult::DATA_OBJECT;
                        result_.dataObject.reference = ref_;
                        size_t lastDot = ref_.rfind('.');
                        if (lastDot != std::string::npos)
                            result_.dataObject.name = ref_.substr(lastDot + 1);
                        else
                            result_.dataObject.name = ref_;

                        // Получаем атрибуты DataObject
                        IedClientError attrError;
                        LinkedList attrList = IedConnection_getDataDirectory(localConn, &attrError, ref_.c_str());
                        if (attrError == IED_ERROR_OK && attrList) {
                            LinkedList entry = attrList;
                            while (entry) {
                                if (entry->data) {
                                    char* attrName = (char*)entry->data;
                                    std::string attrRef = ref_ + "." + attrName;
                                    DataAttributeInfo attr;
                                    attr.name = attrName;
                                    attr.reference = attrRef;
                                    attr.type = "dataAttribute";
                                    attr.functionalConstraint = GetFunctionalConstraintForAttribute(attrName);

                                    // Получаем MMS тип атрибута
                                    IedClientError specError;
                                    FunctionalConstraint fc = IEC61850_FC_ST;
                                    if (attr.functionalConstraint == "CF") fc = IEC61850_FC_CF;
                                    else if (attr.functionalConstraint == "CO") fc = IEC61850_FC_CO;
                                    else if (attr.functionalConstraint == "DC") fc = IEC61850_FC_DC;
                                    else if (attr.functionalConstraint == "MX") fc = IEC61850_FC_MX;

                                    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(
                                        localConn, &specError, attrRef.c_str(), fc);
                                    if (specError == IED_ERROR_OK && spec) {
                                        MmsType mmsType = static_cast<MmsType>(MmsVariableSpecification_getType(spec));
                                        attr.mmsType = MmsTypeToString(mmsType);
                                        MmsVariableSpecification_destroy(spec);
                                    } else {
                                        attr.mmsType = "unknown";
                                    }

                                    result_.dataObject.attributes.push_back(attr);
                                }
                                entry = LinkedList_getNext(entry);
                            }
                            LinkedList_destroy(attrList);
                        }
                    }
                }
            }
        }

        void OnOK() override {
            Napi::Env env = env_;
            switch (result_.type) {
                case BrowseResult::ROOT_NODES: {
                    Napi::Array arr = Napi::Array::New(env, result_.rootNodes.size());
                    for (size_t i = 0; i < result_.rootNodes.size(); ++i) {
                        const auto& ln = result_.rootNodes[i];
                        Napi::Object obj = Napi::Object::New(env);
                        obj.Set("name", Napi::String::New(env, ln.name));
                        obj.Set("reference", Napi::String::New(env, ln.reference));
                        obj.Set("type", Napi::String::New(env, "logicalNode"));

                        Napi::Array dsArr = Napi::Array::New(env, ln.dataSets.size());
                        for (size_t j = 0; j < ln.dataSets.size(); ++j) {
                            Napi::Object dsObj = Napi::Object::New(env);
                            dsObj.Set("name", Napi::String::New(env, ln.dataSets[j].name));
                            dsObj.Set("reference", Napi::String::New(env, ln.dataSets[j].reference));
                            dsObj.Set("type", Napi::String::New(env, ln.dataSets[j].type));
                            dsArr.Set(j, dsObj);
                        }
                        obj.Set("dataSets", dsArr);

                        Napi::Array rptArr = Napi::Array::New(env, ln.reports.size());
                        for (size_t j = 0; j < ln.reports.size(); ++j) {
                            Napi::Object rptObj = Napi::Object::New(env);
                            rptObj.Set("name", Napi::String::New(env, ln.reports[j].name));
                            rptObj.Set("reference", Napi::String::New(env, ln.reports[j].reference));
                            rptObj.Set("type", Napi::String::New(env, ln.reports[j].type));
                            rptObj.Set("description", Napi::String::New(env, ln.reports[j].description));
                            rptArr.Set(j, rptObj);
                        }
                        obj.Set("reports", rptArr);
                        arr.Set(i, obj);
                    }
                    deferred_.Resolve(arr);
                    break;
                }

                case BrowseResult::LOGICAL_NODE: {
                    const auto& ln = result_.logicalNode;
                    Napi::Array resultArray = Napi::Array::New(env, ln.dataObjects.size());
                    for (size_t i = 0; i < ln.dataObjects.size(); ++i) {
                        const auto& dobj = ln.dataObjects[i];
                        Napi::Object obj = Napi::Object::New(env);
                        obj.Set("name", Napi::String::New(env, dobj.name));
                        obj.Set("reference", Napi::String::New(env, dobj.reference));
                        obj.Set("type", Napi::String::New(env, "dataObject"));
                        obj.Set("cdc", Napi::String::New(env, dobj.cdc));
                        resultArray.Set(i, obj);
                    }
                    deferred_.Resolve(resultArray);
                    break;
                }

                case BrowseResult::DATA_OBJECT: {
                    const auto& dobj = result_.dataObject;
                    Napi::Array resultArray = Napi::Array::New(env, dobj.attributes.size());
                    for (size_t i = 0; i < dobj.attributes.size(); ++i) {
                        const auto& attr = dobj.attributes[i];
                        Napi::Object obj = Napi::Object::New(env);
                        obj.Set("name", Napi::String::New(env, attr.name));
                        obj.Set("reference", Napi::String::New(env, attr.reference));
                        obj.Set("type", Napi::String::New(env, "dataAttribute"));
                        obj.Set("FC", Napi::String::New(env, attr.functionalConstraint));
                        obj.Set("mmsType", Napi::String::New(env, attr.mmsType));
                        resultArray.Set(i, obj);
                    }
                    deferred_.Resolve(resultArray);
                    break;
                }

                case BrowseResult::DATA_SET: {
                    const auto& ds = result_.dataSet;
                    Napi::Object obj = Napi::Object::New(env);
                    obj.Set("type", Napi::String::New(env, "dataset"));
                    obj.Set("reference", Napi::String::New(env, ds.reference));
                    obj.Set("name", Napi::String::New(env, ds.name));
                    if (!ds.isValid) {
                        obj.Set("isValid", Napi::Boolean::New(env, false));
                        obj.Set("errorReason", Napi::String::New(env, ds.errorReason));
                    } else {
                        obj.Set("isValid", Napi::Boolean::New(env, true));
                        obj.Set("isDeletable", Napi::Boolean::New(env, ds.isDeletable));
                        obj.Set("alreadyCached", Napi::Boolean::New(env, ds.alreadyCached));
                        obj.Set("memberCount", Napi::Number::New(env, ds.memberCount));

                        Napi::Array membersArr = Napi::Array::New(env, ds.members.size());
                        for (size_t i = 0; i < ds.members.size(); ++i) {
                            const std::string& fullRef = ds.members[i].first;   // например "A01LD0/MT_MMXU1.Hz[MX]"
                            const std::string& simpleName = ds.members[i].second; // "Hz[MX]" (не используется)

                            // Извлекаем чистую ссылку (без FC) и FC-часть
                            std::string refWithoutFc = fullRef;
                            std::string fcPart;
                            size_t bracketPos = fullRef.find('[');
                            if (bracketPos != std::string::npos && fullRef.back() == ']') {
                                fcPart = fullRef.substr(bracketPos);               // "[MX]"
                                refWithoutFc = fullRef.substr(0, bracketPos);      // "A01LD0/MT_MMXU1.Hz"
                            }

                            // Формируем отображаемое имя: удаляем логическое устройство (всё до '/')
                            std::string displayName = refWithoutFc;
                            size_t slashPos = displayName.find('/');
                            if (slashPos != std::string::npos) {
                                displayName = displayName.substr(slashPos + 1);    // "MT_MMXU1.Hz"
                            }

                            // Определяем тип (CDC) на основе FC
                            std::string typeStr;
                            if (fcPart == "[ST]") {
                                // Можно уточнить по имени: если содержит "Pos" -> DPC, иначе SPS
                                if (displayName.find("Pos") != std::string::npos ||
                                    displayName.find("Sw") != std::string::npos)
                                    typeStr = "DPC";
                                else
                                    typeStr = "SPS";
                            } else if (fcPart == "[MX]") {
                                typeStr = "MV";
                            } else if (fcPart == "[CO]") {
                                typeStr = "CO";
                            } else if (fcPart == "[CF]") {
                                typeStr = "CF";
                            } else if (fcPart == "[DC]") {
                                typeStr = "LPL";
                            } else {
                                typeStr = "Unknown";
                            }

                            Napi::Object memberObj = Napi::Object::New(env);
                            memberObj.Set("reference", Napi::String::New(env, refWithoutFc));
                            memberObj.Set("name", Napi::String::New(env, displayName));
                            memberObj.Set("fc", Napi::String::New(env, fcPart));
                            memberObj.Set("type", Napi::String::New(env, typeStr));
                            membersArr.Set(i, memberObj);
                        }
                        obj.Set("members", membersArr);
                    }
                    deferred_.Resolve(obj);
                    break;
                }

                case BrowseResult::REPORT: {
                    const auto& rpt = result_.report;
                    Napi::Object obj = Napi::Object::New(env);
                    obj.Set("type", Napi::String::New(env, "report"));
                    obj.Set("reference", Napi::String::New(env, rpt.reference));
                    obj.Set("name", Napi::String::New(env, rpt.name));
                    if (!rpt.isValid) {
                        obj.Set("isValid", Napi::Boolean::New(env, false));
                        obj.Set("errorReason", Napi::String::New(env, rpt.errorReason));
                    } else {
                        obj.Set("isValid", Napi::Boolean::New(env, true));
                        obj.Set("reportType", Napi::String::New(env, rpt.reportType));
                        obj.Set("datasetRef", Napi::String::New(env, rpt.datasetRef));
                        obj.Set("originalDatasetRef", Napi::String::New(env, rpt.originalDatasetRef));
                        obj.Set("enabled", Napi::Boolean::New(env, rpt.enabled));
                        obj.Set("reportId", Napi::String::New(env, rpt.reportId));
                        obj.Set("trgOps", Napi::Number::New(env, rpt.trgOps));
                        obj.Set("intgPd", Napi::Number::New(env, rpt.intgPd));
                        obj.Set("bufTm", Napi::Number::New(env, rpt.bufTm));
                        obj.Set("gi", Napi::Boolean::New(env, rpt.gi));
                        obj.Set("datasetAlreadyCached", Napi::Boolean::New(env, rpt.datasetAlreadyCached));
                    }
                    deferred_.Resolve(obj);
                    break;
                }

                case BrowseResult::ERROR:
                    deferred_.Reject(Napi::Error::New(env, result_.errorReason).Value());
                    break;
            }
        }

        void OnError(const Napi::Error& e) override {
            deferred_.Reject(e.Value());
        }

    private:
        MmsClient* client_;
        IedConnection connection_;
        std::recursive_timed_mutex& connMutex_;
        Napi::Env env_;
        std::string ref_;
        Napi::Promise::Deferred deferred_;
        BrowseResult result_;
    };

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
                           std::recursive_timed_mutex& connMutex,
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
            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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

        // --- Шаг 2: Рекурсивное кэширование структур (как в старой версии) ---
        // Этот шаг получает имена элементов структур с сервера и заполняет кэш
        for (const auto& memberRef : memberRefs) {
            std::string cleanRef = memberRef;
            FunctionalConstraint fc = IEC61850_FC_ST;
            size_t bracketPos = memberRef.find('[');
            if (bracketPos != std::string::npos && memberRef.back() == ']') {
                std::string fcStr = memberRef.substr(bracketPos + 1, memberRef.length() - bracketPos - 2);
                cleanRef = memberRef.substr(0, bracketPos);
                fc = ParseFCFromString(fcStr);
            }
            // Рекурсивное кэширование структуры (сетевые вызовы без мьютекса)
            RecursiveCacheStructureElements(connection_, client_, cleanRef, fc, 0);
        }

        // --- Шаг 3: Чтение значений DataSet (БЕЗ мьютекса) ---
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

        // --- Шаг 4: Применение кэшированных имён структур (снова под мьютексом) ---
        {
            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
        std::recursive_timed_mutex& connMutex_;
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
    
    class PollDataSetValuesWorker : public Napi::AsyncWorker {
public:
    PollDataSetValuesWorker(MmsClient* client,
                            IedConnection connection,
                            std::recursive_timed_mutex& connMutex,
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
        IedConnection localConn = nullptr;
        bool isConnected = false;
        {
            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
            isConnected = client_->IsConnected();
            localConn = client_->GetConnection();
        }
        if (!isConnected || !localConn) {
            for (const auto& dsRef : datasetRefs_) {
                DataSetPollResult res;
                res.datasetRef = dsRef;
                res.isValid = false;
                res.errorReason = "Not connected";
                results_.push_back(res);
            }
            return;
        }

        for (const auto& dsRef : datasetRefs_) {
            DataSetPollResult res;
            res.datasetRef = dsRef;
            res.isValid = false;

            // Получаем memberRefs из кэша (короткая блокировка)
            std::vector<std::string> memberRefs;
            {
                std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                auto cacheIt = client_->GetDataSetCache().find(dsRef);
                if (cacheIt == client_->GetDataSetCache().end()) {
                    res.errorReason = "DataSet not cached";
                    results_.push_back(res);
                    continue;
                }
                memberRefs = cacheIt->second.memberRefs;
            }

            // Чтение DataSet (без мьютекса)
            IedClientError error;
            ClientDataSet clientDataSet = nullptr;
            auto readStart = std::chrono::steady_clock::now();
            clientDataSet = IedConnection_readDataSetValues(localConn, &error, dsRef.c_str(), nullptr);
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

            // Применение кэшированных имён структур (короткая блокировка)
            {
                std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                for (size_t i = 0; i < rawValues.size(); ++i) {
                    const std::string& fullRef = memberRefs[i];
                    if (rawValues[i].type == MMS_STRUCTURE) {
                        // Вызываем функцию улучшения с передачей полной ссылки
                        EnhanceResultDataWithCachedNames(client_, rawValues[i], fullRef, 0);
                    }
                }
            }

            res.values = std::move(rawValues);
            res.isValid = true;
            res.count = elementsToProcess;
            res.memberRefs = std::move(memberRefs);
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
            } else {
                obj.Set("count", Napi::Number::New(env, res.count));
                obj.Set("readTimeMicros", Napi::Number::New(env, (double)res.readTimeMicros));
                obj.Set("processTimeMicros", Napi::Number::New(env, (double)res.processTimeMicros));
                Napi::Object valuesObj = Napi::Object::New(env);
                for (size_t i = 0; i < res.values.size(); ++i) {
                    Napi::Value jsValue = ResultDataToNapiWithNames(env, res.values[i], res.memberRefs[i]);
                    valuesObj.Set(res.memberRefs[i], jsValue);
                }
                obj.Set("values", valuesObj);
            }
            resultArray.Set((uint32_t)idx, obj);
        }
        deferred_.Resolve(resultArray);
    }
    
    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }
    
private:
    MmsClient* client_;
    IedConnection connection_;
    std::recursive_timed_mutex& connMutex_;
    Napi::Env env_;
    std::vector<std::string> datasetRefs_;
    Napi::Promise::Deferred deferred_;
    std::vector<DataSetPollResult> results_;
};

     // Результат быстрого чтения одного DataRef (poll)
    struct ReadDataResult {
        std::string dataRef;
        bool isValid;
        std::string errorReason;
        MmsClient::ResultData value;
        FunctionalConstraint usedFc;   // какой FC был успешно использован
    };
    
    class ReadDataWorker : public Napi::AsyncWorker {
public:
    ReadDataWorker(MmsClient* client,
                   IedConnection connection,
                   std::recursive_timed_mutex& connMutex,
                   Napi::Env env,
                   const std::vector<std::string>& dataRefs,
                   Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env),
          client_(client),
          connection_(connection),
          connMutex_(connMutex),
          env_(env),
          dataRefs_(dataRefs),
          deferred_(deferred) {}
    
    ~ReadDataWorker() {}
    
    void Execute() override {
        IedConnection localConn = nullptr;
        bool isConnected = false;
        {
            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
            isConnected = client_->IsConnected();
            localConn = client_->GetConnection();
        }
        if (!isConnected || !localConn) {
            for (const auto& ref : dataRefs_) {
                ReadDataResult res;
                res.dataRef = ref;
                res.isValid = false;
                res.errorReason = "Not connected";
                results_.push_back(res);
            }
            return;
        }

        for (const auto& ref : dataRefs_) {
            ReadDataResult res;
            res.dataRef = ref;
            res.isValid = false;

            std::string actualRef;
            FunctionalConstraint requestedFc = IEC61850_FC_ST;
            size_t bracketPos = ref.find('[');
            if (bracketPos != std::string::npos && ref.back() == ']') {
                std::string fcStr = ref.substr(bracketPos + 1, ref.length() - bracketPos - 2);
                requestedFc = ParseFCFromString(fcStr);
                actualRef = ref.substr(0, bracketPos);
            } else {
                actualRef = ref;
            }

            IedClientError error;
            MmsValue* value = nullptr;
            FunctionalConstraint usedFc = requestedFc;
            std::vector<FunctionalConstraint> fallbackFcs = {
                IEC61850_FC_ST, IEC61850_FC_MX, IEC61850_FC_CO,
                IEC61850_FC_CF, IEC61850_FC_DC, IEC61850_FC_SP,
                IEC61850_FC_SG, IEC61850_FC_ALL
            };

            value = IedConnection_readObject(localConn, &error, actualRef.c_str(), requestedFc);
            if (error != IED_ERROR_OK || !value) {
                for (auto tryFc : fallbackFcs) {
                    if (tryFc == requestedFc) continue;
                    if (value) { MmsValue_delete(value); value = nullptr; }
                    value = IedConnection_readObject(localConn, &error, actualRef.c_str(), tryFc);
                    if (error == IED_ERROR_OK && value) {
                        usedFc = tryFc;
                        break;
                    }
                }
            }
            if (error != IED_ERROR_OK || !value) {
                res.errorReason = "Read failed for " + ref + ", error: " + std::to_string(error);
                results_.push_back(res);
                continue;
            }
            if (MmsValue_getType(value) == MMS_DATA_ACCESS_ERROR) {
                res.errorReason = "Data access error for " + ref;
                MmsValue_delete(value);
                results_.push_back(res);
                continue;
            }

            std::string attrName = actualRef;
            size_t lastDot = actualRef.rfind('.');
            if (lastDot != std::string::npos) attrName = actualRef.substr(lastDot + 1);
            MmsClient::ResultData rd = ConvertMmsValueForReportFast(value, attrName, 0);

            // Применяем кэш, если структура
            if (rd.type == MMS_STRUCTURE) {
                std::string fullRefWithFc = actualRef + "[" + FunctionalConstraintToString(usedFc) + "]";
                std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                std::vector<std::string> elementNames;
                if (client_->GetCachedElementNames(fullRefWithFc, elementNames)) {                
                    rd.structureElementNames = elementNames;
                    // Рекурсивно улучшаем вложенные структуры (вызываем функцию)
                    EnhanceResultDataWithCachedNames(client_, rd, fullRefWithFc, 0);
                }
            }

            res.isValid = true;
            res.value = std::move(rd);
            res.usedFc = usedFc;
            MmsValue_delete(value);
            results_.push_back(res);
        }
    }
    
    void OnOK() override {
        Napi::Env env = env_;
        Napi::Array resultArray = Napi::Array::New(env, results_.size());
        for (size_t idx = 0; idx < results_.size(); ++idx) {
            ReadDataResult& res = results_[idx];
            Napi::Object item = Napi::Object::New(env);
            item.Set("dataRef", Napi::String::New(env, res.dataRef));
            item.Set("isValid", Napi::Boolean::New(env, res.isValid));
            if (!res.isValid) {
                item.Set("errorReason", Napi::String::New(env, res.errorReason));
                item.Set("value", Napi::String::New(env, res.errorReason));
            } else {
                Napi::Value jsValue = ResultDataToNapiWithNames(env, res.value, res.dataRef);
                item.Set("value", jsValue);
            }
            resultArray.Set((uint32_t)idx, item);
        }
        deferred_.Resolve(resultArray);
    }
    
    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }
    
private:
    MmsClient* client_;
    IedConnection connection_;
    std::recursive_timed_mutex& connMutex_;
    Napi::Env env_;
    std::vector<std::string> dataRefs_;
    Napi::Promise::Deferred deferred_;
    std::vector<ReadDataResult> results_;
};

    struct DataModelCacheResult {
    std::string dataRef;
    bool isValid;
    std::string errorReason;
    MmsClient::ResultData value; // прочитанное значение (опционально)
};

    class ReadDataModelWorker : public Napi::AsyncWorker {
    public:
        ReadDataModelWorker(MmsClient* client,
                            IedConnection connection,
                            std::recursive_timed_mutex& connMutex,
                            Napi::Env env,
                            const std::vector<std::string>& dataRefs,
                            Napi::Promise::Deferred deferred)
            : Napi::AsyncWorker(env),
            client_(client),
            connection_(connection),
            connMutex_(connMutex),
            env_(env),
            dataRefs_(dataRefs),
            deferred_(deferred) {}

        ~ReadDataModelWorker() {}

        void Execute() override {
            IedConnection localConn = nullptr;
            bool isConnected = false;
            {
                std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                isConnected = client_->IsConnected();
                localConn = client_->GetConnection();
            }
            if (!isConnected || !localConn) {
                for (const auto& ref : dataRefs_) {
                    DataModelCacheResult res;
                    res.dataRef = ref;
                    res.isValid = false;
                    res.errorReason = "Not connected";
                    results_.push_back(res);
                }
                return;
            }

            for (const auto& ref : dataRefs_) {
                DataModelCacheResult res;
                res.dataRef = ref;
                res.isValid = false;

                // Парсим ссылку и FC
                std::string cleanRef = ref;
                FunctionalConstraint fc = IEC61850_FC_ST;
                size_t bracketPos = ref.find('[');
                if (bracketPos != std::string::npos && ref.back() == ']') {
                    std::string fcStr = ref.substr(bracketPos + 1, ref.length() - bracketPos - 2);
                    cleanRef = ref.substr(0, bracketPos);
                    fc = ParseFCFromString(fcStr);
                }

                // 1. Рекурсивное кэширование структуры
                RecursiveCacheStructureElements(localConn, client_, cleanRef, fc, 0);

                // 2. (Опционально) читаем значение
                IedClientError error;
                MmsValue* value = IedConnection_readObject(localConn, &error, cleanRef.c_str(), fc);
                if (error != IED_ERROR_OK || !value) {
                    res.errorReason = "Read failed: " + std::to_string(error);
                    results_.push_back(res);
                    continue;
                }

                if (MmsValue_getType(value) == MMS_DATA_ACCESS_ERROR) {
                    res.errorReason = "Data access error";
                    MmsValue_delete(value);
                    results_.push_back(res);
                    continue;
                }

                // Конвертируем в ResultData (используем ConvertMmsValueForReportFast)
                std::string attrName = cleanRef;
                size_t lastDot = cleanRef.rfind('.');
                if (lastDot != std::string::npos) attrName = cleanRef.substr(lastDot + 1);
                MmsClient::ResultData rd = ConvertMmsValueForReportFast(value, attrName, 0);

                // Применяем кэш (хотя уже должен быть заполнен)
                if (rd.type == MMS_STRUCTURE) {
                    std::string fullRefWithFc = cleanRef + "[" + FunctionalConstraintToString(fc) + "]";
                    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                    std::vector<std::string> elementNames;
                    if (client_->GetCachedElementNames(fullRefWithFc, elementNames)) {
                        rd.structureElementNames = elementNames;
                        EnhanceResultDataWithCachedNames(client_, rd, fullRefWithFc, 0);
                    }
                }

                res.isValid = true;
                res.value = std::move(rd);
                MmsValue_delete(value);

                results_.push_back(res);
            }
        }

        void OnOK() override {
            Napi::Env env = env_;
            Napi::Array resultArray = Napi::Array::New(env, results_.size());
            for (size_t idx = 0; idx < results_.size(); ++idx) {
                DataModelCacheResult& res = results_[idx];
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("dataRef", Napi::String::New(env, res.dataRef));
                obj.Set("isValid", Napi::Boolean::New(env, res.isValid));
                if (!res.isValid) {
                    obj.Set("errorReason", Napi::String::New(env, res.errorReason));
                } else {
                    Napi::Value jsValue = ResultDataToNapiWithNames(env, res.value, res.dataRef);
                    obj.Set("value", jsValue);
                }
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
        std::recursive_timed_mutex& connMutex_;
        Napi::Env env_;
        std::vector<std::string> dataRefs_;
        Napi::Promise::Deferred deferred_;
        std::vector<DataModelCacheResult> results_;
    };
} // анонимный namespace

void MmsClient::AddToStructureCache(const std::string& fullRef,
                                    const std::vector<std::string>& elementNames,
                                    const std::vector<MmsType>& elementTypes) {
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    
    StructureElementNames info;
    info.ref = fullRef;
    info.elementNames = elementNames;
    info.elementTypes = elementTypes;
    
    // Сохраняем в глобальный кэш
    globalStructureCache_[fullRef] = info;
    
    // Также пробуем сохранить в кэш DataSet, если найдётся подходящий
    for (auto& [dsRef, cache] : datasetCache_) {
        for (const auto& memberRef : cache.memberRefs) {
            std::string cleanMemberRef = memberRef;
            size_t bracketPos = memberRef.find('[');
            if (bracketPos != std::string::npos) {
                cleanMemberRef = memberRef.substr(0, bracketPos);
            }
            if (cleanMemberRef == fullRef.substr(0, fullRef.find('['))) {
                cache.structureCache[fullRef] = info;
                break;
            }
        }
    }
}

Napi::FunctionReference MmsClient::constructor;

struct ConnectionHandlerContext {
    MmsClient* client;
    std::recursive_timed_mutex* mutex;  
};

// Структура для хранения информации о структуре (определена, например, в заголовке)
struct StructureInfo {
    std::vector<std::string> elementNames;
    std::vector<MmsType> elementTypes;
};



void MmsClient::CheckConnectionHealth() {
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    
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
/*static void EnhanceStructureWithCachedNames(MmsClient::ResultData& data,
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
}*/

static void EnhanceStructureWithCachedNames(MmsClient* client,
                                            MmsClient::ResultData& data,
                                            const std::string& fullRef,
                                            int depth = 0) {
    const int MAX_DEPTH = 5;
    if (depth > MAX_DEPTH || data.type != MMS_STRUCTURE) return;

    std::vector<std::string> elementNames;
    if (client->GetCachedElementNames(fullRef, elementNames) &&
        elementNames.size() == data.structureElements.size()) {
        data.structureElementNames = elementNames;

        // Извлекаем чистую ссылку и часть с FC
        std::string cleanRef = fullRef;
        std::string fcPart;
        size_t bracketPos = fullRef.find('[');
        if (bracketPos != std::string::npos) {
            cleanRef = fullRef.substr(0, bracketPos);
            fcPart = fullRef.substr(bracketPos);
        }

        for (size_t i = 0; i < data.structureElements.size(); ++i) {
            std::string childRef = cleanRef + "." + elementNames[i] + fcPart;
            EnhanceStructureWithCachedNames(client, data.structureElements[i], childRef, depth + 1);
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
/*static void RecursiveCacheStructureElements(IedConnection connection,
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
        std::lock_guard<std::recursive_timed_mutex> lock(client->GetMutex());           
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
}*/

static void RecursiveCacheStructureElements(IedConnection connection,
                                          MmsClient* client,
                                          const std::string& baseRef,
                                          FunctionalConstraint fc,
                                          int recursionDepth) {
    const int MAX_CACHE_DEPTH = 5;
    if (recursionDepth > MAX_CACHE_DEPTH) return;

    IedClientError error;
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(connection, &error, baseRef.c_str(), fc);
    if (error != IED_ERROR_OK || !spec) return;

    int type = MmsVariableSpecification_getType(spec);
    if (type == MMS_STRUCTURE) {
        int size = MmsVariableSpecification_getSize(spec);
        std::vector<std::string> elementNames;
        std::vector<MmsType> elementTypes;
        for (int i = 0; i < size; ++i) {
            MmsVariableSpecification* child = MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
            if (child) {
                const char* name = MmsVariableSpecification_getName(child);
                if (name) {
                    elementNames.push_back(name);
                    MmsType childType = static_cast<MmsType>(MmsVariableSpecification_getType(child));
                    elementTypes.push_back(childType);
                    if (childType == MMS_STRUCTURE) {
                        std::string childRef = baseRef + "." + name;
                        RecursiveCacheStructureElements(connection, client, childRef, fc, recursionDepth + 1);
                    }
                }
            }
        }
        if (!elementNames.empty()) {
            client->CacheStructureElements(baseRef, fc, elementNames, elementTypes);
        }
    }
    MmsVariableSpecification_destroy(spec);
}

void MmsClient::CacheDataSetStructure(const std::string& datasetRef,
                                       const std::vector<std::string>& memberRefs) {
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    if (datasetCache_.find(datasetRef) != datasetCache_.end()) {
        return; // уже закэширован
    }
    DataSetCache cache;
    cache.datasetRef = datasetRef;
    cache.memberRefs = memberRefs;
    datasetCache_[datasetRef] = std::move(cache);
}

/*bool MmsClient::GetCachedElementNames(const std::string& fullRef, std::vector<std::string>& elementNames) {
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    
    printf("GetCachedElementNames: searching for '%s'\n", fullRef.c_str());
    
    for (const auto& [dsRef, cache] : datasetCache_) {
        auto it = cache.structureCache.find(fullRef);
        if (it != cache.structureCache.end()) {
            elementNames = it->second.elementNames;
            printf("  FOUND in dataset '%s', %zu names: ", dsRef.c_str(), elementNames.size());
            for (size_t i = 0; i < elementNames.size() && i < 5; ++i) {
                printf("%s ", elementNames[i].c_str());
            }
            if (elementNames.size() > 5) printf("...");
            printf("\n");
            return true;
        }
    }
    printf("  NOT FOUND\n");
    return false;
}*/

bool MmsClient::GetCachedElementNames(const std::string& fullRef, 
                                      std::vector<std::string>& elementNames) {
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    
    // 1. Поиск в глобальном кэше
    auto itGlobal = globalStructureCache_.find(fullRef);
    if (itGlobal != globalStructureCache_.end()) {
        elementNames = itGlobal->second.elementNames;
        return true;
    }
    
    // 2. Поиск в кэшах DataSet
    for (const auto& [dsRef, cache] : datasetCache_) {
        auto it = cache.structureCache.find(fullRef);
        if (it != cache.structureCache.end()) {
            elementNames = it->second.elementNames;
            return true;
        }
    }
    
    // 3. Не найдено – пробуем найти без учёта FC (если fullRef содержит FC, убираем его)
    size_t bracketPos = fullRef.find('[');
    if (bracketPos != std::string::npos) {
        std::string cleanRef = fullRef.substr(0, bracketPos);
        for (const auto& [dsRef, cache] : datasetCache_) {
            for (const auto& [cachedRef, info] : cache.structureCache) {
                if (cachedRef.find(cleanRef) == 0) {
                    elementNames = info.elementNames;
                    return true;
                }
            }
        }
        // Также поиск в глобальном кэше по чистому имени
        for (const auto& [cachedRef, info] : globalStructureCache_) {
            if (cachedRef.find(cleanRef) == 0) {
                elementNames = info.elementNames;
                return true;
            }
        }
    }
    
    return false;
}

// Метод для кэширования имен элементов структуры
/*void MmsClient::CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
                                      const std::vector<std::string>& elementNames,
                                      const std::vector<MmsType>& elementTypes) {
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    
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
}*/

void MmsClient::CacheStructureElements(const std::string& ref, FunctionalConstraint fc,
                                      const std::vector<std::string>& elementNames,
                                      const std::vector<MmsType>& elementTypes) {
    // Формируем полный ключ: ссылка + "[" + FC_строка + "]"
    std::string fcStr = FunctionalConstraintToString(fc);
    std::string fullRef = ref + "[" + fcStr + "]";
    
    // Добавляем в кэш (глобальный и, если возможно, в DataSet)
    AddToStructureCache(fullRef, elementNames, elementTypes);
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
        hasCachedNames = client->GetCachedElementNames(cleanRef, elementNames);
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
        InstanceMethod("readDataModel", &MmsClient::ReadDataModel),
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
    
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
    //std::recursive_timed_mutex* mutex = context->mutex;

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
        std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
                    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
                            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
                            if (!connected_ || !running_) break;
                        }

                        IedConnectionState currentState = IedConnection_getState(connection_);
                        if (currentState != IED_STATE_CONNECTED) {
                            printf("Connection lost, state: %d, clientID: %s\n", currentState, clientID_.c_str());
                            
                            {
                                ////std::lock_guard<std::mutex> lock(connMutex_);
                                std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
                                    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
                            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
            std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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

/*static void EnhanceResultDataWithCachedNames(MmsClient* client,
                                             MmsClient::ResultData& data,
                                             const std::string& fullRef,
                                             int depth = 0) {
    const int MAX_DEPTH = 5;
    if (depth > MAX_DEPTH || data.type != MMS_STRUCTURE) {
        if (depth > MAX_DEPTH) {
            printf("EnhanceResultDataWithCachedNames: max depth reached for %s\n", fullRef.c_str());
        }
        return;
    }
    
    printf("EnhanceResultDataWithCachedNames: depth=%d, ref='%s', struct size=%zu\n", 
           depth, fullRef.c_str(), data.structureElements.size());

    std::vector<std::string> elementNames;
    if (client->GetCachedElementNames(fullRef, elementNames) &&
        elementNames.size() == data.structureElements.size()) {
        
        printf("  Applying cached names for %s\n", fullRef.c_str());
        data.structureElementNames = elementNames;

        // Извлекаем чистую ссылку и часть с FC
        std::string cleanRef = fullRef;
        std::string fcPart;
        size_t bracketPos = fullRef.find('[');
        if (bracketPos != std::string::npos && fullRef.back() == ']') {
            cleanRef = fullRef.substr(0, bracketPos);
            fcPart = fullRef.substr(bracketPos);
        }

        for (size_t i = 0; i < data.structureElements.size(); ++i) {
            std::string childRef = cleanRef + "." + elementNames[i] + fcPart;
            printf("  Recursing into child %zu: %s\n", i, childRef.c_str());
            EnhanceResultDataWithCachedNames(client, data.structureElements[i], childRef, depth + 1);
        }
    } else {
        printf("  No cache or size mismatch for %s (cache size=%zu, struct size=%zu)\n", 
               fullRef.c_str(), elementNames.size(), data.structureElements.size());
    }
}*/

static void EnhanceResultDataWithCachedNames(MmsClient* client,
                                             MmsClient::ResultData& data,
                                             const std::string& fullRef,
                                             int depth) {
    const int MAX_DEPTH = 5;
    if (depth > MAX_DEPTH || data.type != MMS_STRUCTURE) return;

    std::vector<std::string> elementNames;
    if (client->GetCachedElementNames(fullRef, elementNames) &&
        elementNames.size() == data.structureElements.size()) {
        data.structureElementNames = elementNames;

        // Извлекаем чистую ссылку и часть с FC для рекурсии
        std::string cleanRef = fullRef;
        std::string fcPart;
        size_t bracketPos = fullRef.find('[');
        if (bracketPos != std::string::npos && fullRef.back() == ']') {
            cleanRef = fullRef.substr(0, bracketPos);
            fcPart = fullRef.substr(bracketPos);
        }

        for (size_t i = 0; i < data.structureElements.size(); ++i) {
            std::string childRef = cleanRef + "." + elementNames[i] + fcPart;
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

Napi::Value MmsClient::ReadDataModel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || (!info[0].IsString() && !info[0].IsArray())) {
        Napi::TypeError::New(env, "Expected string or array of strings").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::vector<std::string> dataRefs;
    if (info[0].IsString()) {
        dataRefs.push_back(info[0].As<Napi::String>().Utf8Value());
    } else {
        Napi::Array arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            if (arr.Get(i).IsString()) {
                dataRefs.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
            }
        }
    }

    if (dataRefs.empty()) {
        Napi::TypeError::New(env, "No valid data references provided").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // Быстрая проверка соединения
    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    auto deferred = Napi::Promise::Deferred::New(env);
    ReadDataModelWorker* worker = new ReadDataModelWorker(
        this,
        connection_,
        connMutex_,
        env,
        dataRefs,
        deferred
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

Napi::Value MmsClient::BrowseDataModel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Null();
    }

    // Определяем строку ссылки (если есть)
    std::string ref;
    if (info.Length() > 0 && info[0].IsString()) {
        ref = info[0].As<Napi::String>().Utf8Value();
    }

    auto deferred = Napi::Promise::Deferred::New(env);
    BrowseDataModelWorker* worker = new BrowseDataModelWorker(
        this,
        connection_,
        connMutex_,
        env,
        ref,
        deferred
    );
    worker->Queue();
    return deferred.Promise();
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
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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

    // Проверка входных параметров (как и раньше)
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

    if (dataRefs.empty()) {
        Napi::TypeError::New(env, "No valid data references provided").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // Быстрая проверка соединения (без длительной блокировки)
    if (!connected_) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // Создаём Promise
    auto deferred = Napi::Promise::Deferred::New(env);

    // Создаём и запускаем воркер
    ReadDataWorker* worker = new ReadDataWorker(
        this,
        connection_,
        connMutex_,
        env,
        dataRefs,
        deferred
    );

    worker->Queue();
    return deferred.Promise();
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
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
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
    
    // Пытаемся захватить мьютекс с таймаутом 100 мс
    std::unique_lock<std::recursive_timed_mutex> lock(client->connMutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(100))) {
        printf("ReportCallback: mutex busy, skipping report (client %s)\n", client->clientID_.c_str());
        return;
    }
    
    // Теперь мьютекс захвачен, можно безопасно работать с разделяемыми данными
    if (client->isClosing_ || !client->connected_) {
        printf("ReportCallback: client closing or disconnected, skipping\n");
        return;
    }
    
    auto startTime = std::chrono::steady_clock::now();
    client->totalReportsProcessed_++;
    int currentReport = client->totalReportsProcessed_.load();
    
    const char* rcbRefRaw = ClientReport_getRcbReference(report);
    const char* rptIdRaw = ClientReport_getRptId(report);
    if (!rcbRefRaw) {
        printf("ReportCallback: no RCB reference\n");
        return;
    }
    std::string rcbRef(rcbRefRaw);
    std::string rptId = rptIdRaw ? rptIdRaw : "unknown";
    
    printf("\n=== ReportCallback [REPORT#%d] ===\n", currentReport);
    printf("  RCB: %s, rptId: %s\n", rcbRef.c_str(), rptId.c_str());
    
    auto it = client->activeReports_.find(rcbRef);
    if (it == client->activeReports_.end()) {
        printf("  WARNING: ReportInfo not found for %s\n", rcbRef.c_str());
        return;
    }
    ReportInfo& reportInfo = it->second;
    
    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);
    if (!dataSetValues) {
        printf("  ERROR: dataSetValues is NULL\n");
        return;
    }
    
    int dataSetSize = MmsValue_getArraySize(dataSetValues);
    const std::vector<std::string>& dataSetMembers = reportInfo.dataSetMembers;
    int elementsToProcess = std::min(dataSetSize, (int)dataSetMembers.size());
    
    bool hasTimestamp = ClientReport_hasTimestamp(report);
    uint64_t timestamp = hasTimestamp ? ClientReport_getTimestamp(report) : 0;
    
    struct ReportItemData {
        std::string fullRef;
        MmsClient::ResultData resultData;
        int reason;
    };
    std::vector<ReportItemData> reportItems;
    reportItems.reserve(elementsToProcess);
    
    // Рекурсивная функция обработки значений (без дополнительных блокировок, т.к. мьютекс уже удерживается)
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
        
        std::string attrName = fullRef;
        size_t dotPos = fullRef.rfind('.');
        if (dotPos != std::string::npos) attrName = fullRef.substr(dotPos + 1);
        
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
            if (isStatusStructure && size >= 3) {
                const char* stdNames[] = {"stVal", "q", "t"};
                for (int i = 0; i < std::min(size, 3); ++i) {
                    MmsValue* childVal = MmsValue_getElement(val, i);
                    if (childVal) {
                        std::string childFullRef = fullRef;
                        if (bracketPos != std::string::npos) {
                            childFullRef = fullRef.substr(0, bracketPos) + "." + stdNames[i] + fullRef.substr(bracketPos);
                        } else {
                            childFullRef = fullRef + "." + stdNames[i];
                        }
                        MmsClient::ResultData childData = processValueRecursive(childVal, childFullRef, recursionDepth + 1);
                        data.structureElements.push_back(childData);
                        data.structureElementNames.push_back(stdNames[i]);
                    }
                }
            } else {
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
            data = ConvertMmsValueForReportFast(val, attrName);
        }
        return data;
    };
    
    for (int i = 0; i < elementsToProcess; ++i) {
        ReasonForInclusion reason = ClientReport_getReasonForInclusion(report, i);
        if (reason == IEC61850_REASON_NOT_INCLUDED) continue;
        
        const std::string& fullRef = dataSetMembers[i];
        MmsValue* value = MmsValue_getElement(dataSetValues, i);
        if (!value) continue;
        
        try {
            MmsClient::ResultData rd = processValueRecursive(value, fullRef, 0);
            ReportItemData item{fullRef, std::move(rd), reason};
            reportItems.push_back(std::move(item));
        } catch (const std::exception& e) {
            printf("    Exception processing %s: %s\n", fullRef.c_str(), e.what());
        }
    }
    
    // Применяем кэшированные имена структур (уже под мьютексом)
    for (auto& item : reportItems) {
        if (item.resultData.type == MMS_STRUCTURE) {
            EnhanceStructureWithCachedNames(client, item.resultData, item.fullRef, 0);
        }
    }
    
    // Отправка в JS через TSFN (копируем данные)
    if (!reportItems.empty() && client->tsfn_) {
        auto status = client->tsfn_.NonBlockingCall([client, rcbRef, rptId, timestamp, hasTimestamp, reportItems, currentReport]
                                                    (Napi::Env env, Napi::Function cb) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, client->clientID_));
                eventObj.Set("type", "data");
                eventObj.Set("event", "report");
                eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
                eventObj.Set("rptId", Napi::String::New(env, rptId));
                if (hasTimestamp) eventObj.Set("timestamp", Napi::Number::New(env, (double)timestamp));
                
                Napi::Object valuesObj = Napi::Object::New(env);
                Napi::Object reasonsObj = Napi::Object::New(env);
                for (const auto& item : reportItems) {
                    Napi::Value jsValue = ResultDataToNapiWithNames(env, item.resultData, item.fullRef);
                    valuesObj.Set(item.fullRef, jsValue);
                    reasonsObj.Set(item.fullRef, item.reason);
                }
                eventObj.Set("values", valuesObj);
                eventObj.Set("reasons", reasonsObj);
                eventObj.Set("reportNumber", Napi::Number::New(env, currentReport));
                eventObj.Set("itemsInReport", Napi::Number::New(env, (uint32_t)reportItems.size()));
                eventObj.Set("totalElementsProcessed", Napi::Number::New(env, MmsClient::totalElementsProcessed_.load()));
                
                cb.Call({Napi::String::New(env, "data"), eventObj});
            } catch (const std::exception& e) {
                printf("  TSFN callback exception: %s\n", e.what());
            }
        });
        if (status != napi_ok) {
            printf("  Failed to queue report to TSFN\n");
        }
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    printf("  Report processing time: %lld ms\n", duration.count());
    printf("=== ReportCallback END ===\n\n");
}

Napi::Value MmsClient::EnableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string) and datasetRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();
    std::string datasetRef = info[1].As<Napi::String>().Utf8Value();
    
    // 1. Короткая блокировка – копируем connection_ и проверяем состояние
    IedConnection localConn = nullptr;
    bool isConnected = false;
    bool alreadyEnabled = false;
    {
        std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
        isConnected = connected_;
        localConn = connection_;
        if (activeReports_.find(rcbRef) != activeReports_.end()) {
            alreadyEnabled = true;
        }
    }
    if (!isConnected || !localConn) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (alreadyEnabled) {
        Napi::Error::New(env, "Report already enabled for " + rcbRef).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // 2. Длительные операции без мьютекса
    IedClientError error;
    
    // Получаем директорию DataSet
    bool isDeletable = false;
    LinkedList dataSetDirectory = IedConnection_getDataSetDirectory(localConn, &error, datasetRef.c_str(), &isDeletable);
    if (error != IED_ERROR_OK || !dataSetDirectory) {
        Napi::Error::New(env, "Failed to get dataset directory: " + std::to_string(error)).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::vector<std::string> dataSetMembers;
    LinkedList entry = dataSetDirectory;
    while (entry) {
        if (entry->data) {
            dataSetMembers.push_back(std::string((char*)entry->data));
        }
        entry = LinkedList_getNext(entry);
    }
    LinkedList_destroy(dataSetDirectory);
    
    // Читаем DataSet
    ClientDataSet clientDataSet = IedConnection_readDataSetValues(localConn, &error, datasetRef.c_str(), nullptr);
    if (error != IED_ERROR_OK || !clientDataSet) {
        Napi::Error::New(env, "Failed to read dataset: " + std::to_string(error)).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // Получаем RCB
    ClientReportControlBlock rcb = IedConnection_getRCBValues(localConn, &error, rcbRef.c_str(), nullptr);
    if (error != IED_ERROR_OK || !rcb) {
        ClientDataSet_destroy(clientDataSet);
        Napi::Error::New(env, "Failed to get RCB: " + std::to_string(error)).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // 3. Сбор информации о структурах (без мьютекса)
    //std::unordered_map<std::string, StructureInfo> tempStructCache;
    std::unordered_map<std::string, StructureElementNames> tempStructCache;
    for (const auto& memberRef : dataSetMembers) {
        std::string cleanRef = memberRef;
        FunctionalConstraint fc = IEC61850_FC_ST;
        size_t bracketPos = memberRef.find('[');
        if (bracketPos != std::string::npos && memberRef.back() == ']') {
            std::string fcStr = memberRef.substr(bracketPos + 1, memberRef.length() - bracketPos - 2);
            cleanRef = memberRef.substr(0, bracketPos);
            fc = ParseFCFromString(fcStr);
        }
        CollectStructureInfo(localConn, cleanRef, fc, tempStructCache, 0);
    }
    
    // 4. Настройка RCB (без мьютекса)
    if (ClientReportControlBlock_getResv(rcb)) {
        ClientReportControlBlock_setResv(rcb, false);
        IedConnection_setRCBValues(localConn, &error, rcb, RCB_ELEMENT_RESV, true);
    }
    ClientReportControlBlock_setDataSetReference(rcb, datasetRef.c_str());
    ClientReportControlBlock_setTrgOps(rcb, TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_INTEGRITY);
    ClientReportControlBlock_setIntgPd(rcb, 10000);
    ClientReportControlBlock_setBufTm(rcb, 5000);
    ClientReportControlBlock_setGI(rcb, false);
    ClientReportControlBlock_setRptEna(rcb, true);
    
    uint32_t mask = RCB_ELEMENT_DATSET | RCB_ELEMENT_TRG_OPS | RCB_ELEMENT_INTG_PD;
    IedConnection_setRCBValues(localConn, &error, rcb, mask, true);
    if (error != IED_ERROR_OK) {
        // Fallback: только включение отчёта
        ClientReportControlBlock_setRptEna(rcb, false);
        IedConnection_setRCBValues(localConn, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
        ClientReportControlBlock_setRptEna(rcb, true);
        IedConnection_setRCBValues(localConn, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
    }
    if (error != IED_ERROR_OK) {
        ClientDataSet_destroy(clientDataSet);
        ClientReportControlBlock_destroy(rcb);
        Napi::Error::New(env, "Failed to set RCB values: " + std::to_string(error)).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // 5. Короткая блокировка – сохраняем всё в activeReports_ и кэш
    {
        std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
        if (activeReports_.find(rcbRef) != activeReports_.end()) {
            ClientDataSet_destroy(clientDataSet);
            ClientReportControlBlock_destroy(rcb);
            Napi::Error::New(env, "Report already enabled (race condition)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        
        ReportInfo reportInfo;
        reportInfo.rcbRef = rcbRef;
        reportInfo.datasetRef = datasetRef;
        reportInfo.dataSetMembers = std::move(dataSetMembers);
        reportInfo.rcb = rcb;
        reportInfo.dataSet = clientDataSet;
        for (auto& [ref, info] : tempStructCache) {
            datasetCache_[datasetRef].structureCache[ref] = std::move(info);
        }
        activeReports_[rcbRef] = std::move(reportInfo);
    }
    
    // 6. Установка обработчика отчёта
    IedConnection_installReportHandler(localConn, rcbRef.c_str(),
                                       ClientReportControlBlock_getRptId(rcb),
                                       ReportCallback, this);
    
    // 7. Отправка события
    tsfn_.BlockingCall([this, rcbRef, datasetRef, memberCount = (int)dataSetMembers.size()](Napi::Env env, Napi::Function jsCallback) {
        try {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, clientID_));
            eventObj.Set("type", "control");
            eventObj.Set("event", "reportingEnabled");
            eventObj.Set("rcbRef", Napi::String::New(env, rcbRef));
            eventObj.Set("datasetRef", Napi::String::New(env, datasetRef));
            eventObj.Set("memberCount", Napi::Number::New(env, memberCount));
            jsCallback.Call({Napi::String::New(env, "data"), eventObj});
        } catch (...) {}
    });
    
    printf("EnableReporting: SUCCESS for %s -> %s\n", rcbRef.c_str(), datasetRef.c_str());
    return env.Undefined();
}

Napi::Value MmsClient::DisableReporting(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected rcbRef (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string rcbRef = info[0].As<Napi::String>().Utf8Value();
    
    std::lock_guard<std::recursive_timed_mutex> lock(connMutex_);
    
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