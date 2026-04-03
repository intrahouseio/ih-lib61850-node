const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

const client = new MmsClient((event, data) => {
    console.log(`Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    if (event === 'conn' && data.event === 'opened') {
        console.log('Connection opened');
        // Теперь пользователь сам решит, как исследовать модель
        console.log('\n=== Примеры использования: ===');
        console.log('1. Получить корневые узлы:');
        console.log('   client.browseDataModel()');
        console.log('\n2. Получить DataObjects конкретного узла:');
        console.log('   client.browseDataModel("WAGO61850ServerDevice/LLN0")');
        console.log('\n3. Получить атрибуты DataObject:');
        console.log('   client.browseDataModel("WAGO61850ServerDevice/XCBR1.Pos")');
        console.log('\n4. Получить члены DataSet:');
        console.log('   client.browseDataModel("WAGO61850ServerDevice/LLN0.DataSet01")');
        console.log('\n5. Получить информацию об отчете:');
        console.log('   client.browseDataModel("WAGO61850ServerDevice/LLN0.RP$ReportBlock0101")');
        
        // Автоматически получаем корневые узлы для примера
        exploreModel();
    }

    if (event === 'data' && data.type === 'data') {
        if (data.event === 'logicalDevices') {
            console.log(`Logical Devices received: ${util.inspect(data.logicalDevices, { depth: null })}`);
        } else if (data.event === 'dataSetDirectory') {
            console.log(`DataSet Directory for ${data.logicalNodeRef}: ${util.inspect(data.dataSets, { depth: null })}`);
        } else if (data.event === 'dataModel') {
            console.log(`Data Model received: ${util.inspect(data.dataModel, { depth: null })}`);
        } else if (data.event === 'dataSet') {
            console.log(`DataSet received for ${data.datasetRef}: ${util.inspect(data.value, { depth: null })}`);
        } else if (data.event === 'multipleDataSets') {
            console.log(`Multiple DataSets received for ${util.inspect(data.datasetRefs, { depth: null })}:`);
            data.values.forEach((value, index) => {
                if (value.isValid !== false) {
                    console.log(` DataSet[${index}]: ${data.datasetRefs[index]}, Value: ${util.inspect(value, { depth: null })}`);
                } else {
                    console.log(` DataSet[${index}]: ${data.datasetRefs[index]}, Error: ${value.errorReason}`);
                }
            });
        } else if (data.event === 'report') {
            console.log(`Report received for ${data.rcbRef} (rptId: ${data.rptId}):`);
            if (data.timestamp) {
                console.log(` Timestamp: ${data.timestamp}`);
            }
            Object.entries(data.values).forEach(([ref, value], index) => {
                const reason = data.reasons[ref];
                if (reason && reason !== 0) {
                    console.log(` ${ref}: ${util.inspect(value, { depth: null })}, Reason: ${reason}`);
                }
            });
            
        } else if (data.event === 'batchData') {
            console.log(`Batch Data received for ${util.inspect(data.dataRefs, { depth: null })}:`);
            data.values.forEach((result, index) => {
                if (result.isValid) {
                    console.log(` dataRef[${index}]: ${data.dataRefs[index]}, Value: ${util.inspect(result.value, { depth: null })}`);
                } else {
                    console.log(` dataRef[${index}]: ${data.dataRefs[index]}, Error: ${result.errorReason}`);
                }
            });
        } else {
            console.log(`Data received for ${data.dataRef || 'undefined'}: ${util.inspect(data.value, { depth: null })}`);
        }
    }

    if (event === 'data' && data.type === 'error') {
        console.error(`Error received: ${data.reason}`);
    }

    if (event === 'conn' && data.event === 'reconnecting') {
        console.error(`Reconnection failed: ${data.reason}`);
        if (data.reason.includes('attempt 3')) {
            throw new Error('Max reconnection attempts reached');
        }
    }

    if (event === 'data' && data.type === 'control') {
        if (data.event === 'reportingEnabled') {
            console.log(`Reporting enabled for ${data.rcbRef}`);
        } else if (data.event === 'reportingDisabled') {
            console.log(`Reporting disabled for ${data.rcbRef}`);
        } else if (data.event === 'dataSetCreated') {
            console.log(`DataSet created: ${data.datasetRef}`);
        } else if (data.event === 'dataSetDeleted') {
            console.log(`DataSet deleted: ${data.datasetRef}`);
        } else if (data.event === 'stateChanged') {
            console.log(`Connection state changed: ${data.state}, isConnected: ${data.isConnected}`);
        }
    }

    if (event === 'conn' && data.event === 'stateChanged') {
        console.log(`Connection state changed: ${data.state}, isConnected: ${data.isConnected}`);
    }
});

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

/*async function handleConnectionOpened() {
    try {
        const dataModel = await client.browseDataModel();
        console.log('Data Model:', util.inspect(dataModel, { depth: null }));

        const dataSets = [];
        dataModel.forEach(ld => {
            ld.logicalNodes.forEach(ln => {
                ln.dataSets.forEach(ds => {
                    console.log(`Found dataset: ${ds.reference}`);
                    dataSets.push(ds);
                });
            });
        });

        console.log('\nЧтение значений DataSet...');
        const datasetRefs = dataSets.map(ds => ds.reference);
        console.log('Calling readDataSetValues...');
        const readResults = await client.readDataSetValues(datasetRefs);
        console.log('readDataSetValues returned');
      

        readResults.forEach((res, idx) => {
            const ds = dataSets[idx];
            console.log(`\nDataSet: ${res.datasetRef}`);
            if (!res.isValid) {
                console.error('  Ошибка:', res.errorReason);
                return;
            }

            console.log(`  Значений: ${res.count}, Удаляемые: ${res.isDeletable}`);
            
            // Функция для рекурсивного вывода вложенных структур
            /*const printValue = (value, indent = '  ') => {
                if (value && typeof value === 'object' && !Array.isArray(value)) {
                    Object.entries(value).forEach(([key, val]) => {
                        if (val && typeof val === 'object' && !Array.isArray(val)) {
                            console.log(`${indent}${key}: {`);
                            printValue(val, indent + '  ');
                            console.log(`${indent}}`);
                        } else if (Array.isArray(val)) {
                            console.log(`${indent}${key}: [`);
                            val.forEach((item, i) => {
                                console.log(`${indent}  [${i}]:`);
                                printValue(item, indent + '    ');
                            });
                            console.log(`${indent}]`);
                        } else {
                            console.log(`${indent}${key}: ${util.inspect(val, { colors: true })}`);
                        }
                    });
                } else {
                    console.log(`${indent}${util.inspect(value, { colors: true })}`);
                }
            };

            Object.entries(res.values).forEach(([ref, value]) => {
                console.log(`  ${ref}:`);
                printValue(value, '    ');
            });
            

            // Функция для рекурсивного вывода с полным отображением всех элементов
            const printModel = (model, indent = '') => {
                if (Array.isArray(model)) {
                    model.forEach((item, index) => {
                        console.log(`${indent}[${index}]:`);
                        printModel(item, indent + '  ');
                    });
                } else if (model && typeof model === 'object') {
                    Object.entries(model).forEach(([key, value]) => {
                        if (key === 'dataObjects' && Array.isArray(value)) {
                            console.log(`${indent}${key}: [`);
                            value.forEach((doObj, idx) => {
                                console.log(`${indent}  [${idx}]:`);
                                printModel(doObj, indent + '    ');
                            });
                            console.log(`${indent}]`);
                        } else if (key === 'attributes' && typeof value === 'object') {
                            console.log(`${indent}${key}: {`);
                            printModel(value, indent + '  ');
                            console.log(`${indent}}`);
                        } else if (Array.isArray(value)) {
                            console.log(`${indent}${key}: [`);
                            value.forEach((item, idx) => {
                                console.log(`${indent}  [${idx}]: ${item}`);
                            });
                            console.log(`${indent}]`);
                        } else if (typeof value === 'object') {
                            console.log(`${indent}${key}: {`);
                            printModel(value, indent + '  ');
                            console.log(`${indent}}`);
                        } else {
                            console.log(`${indent}${key}: ${value}`);
                        }
                    });
                } else {
                    console.log(`${indent}${model}`);
                }
            };
           
            });

        console.log('Reading data...');
        const dataRefs = [
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/GGIO1.Ind1.stVal',
            'WAGO61850ServerDevice/CALH12.GrAlm.stVal'
        ];
        const readRefResult = await client.readData(dataRefs); 
        console.log("readRefResult " + util.inspect(readRefResult, { depth: null }));

        const rcbRef2 = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0201';
        const dataSetRef2 = 'WAGO61850ServerDevice/LLN0.DataSet02';
        console.log(`Enabling reporting for ${rcbRef2} with dataset ${dataSetRef2}`);
        await client.enableReporting(rcbRef2, dataSetRef2);

        /*console.log('Reading data...');
        const dataRefs = [
            'A01LD0/Q1_XCBR1.Pos[ST]',
            'A01LD0/In_GGIO1.Ind1',
            'A01LD0/CALH1.GrAlm.stVal'
        ];
        const readRefResult = await client.readData(dataRefs); 
        console.log("readRefResult " + util.inspect(readRefResult, { depth: null }));*/

       /* const rcbRef = 'A01LD0/LLN0.RP.repTI1';
        const dataSetRef = 'A01LD0/LLN0.TI_ASU';
        console.log(`Enabling reporting for ${rcbRef} with dataset ${dataSetRef}`);
        await client.enableReporting(rcbRef, dataSetRef);*/

        /*const rcbRef2 = 'A01LD0/LLN0.BR.repTS1';
        const dataSetRef2 = 'A01LD0/LLN0.TS_ASU';
        console.log(`Enabling reporting for ${rcbRef2} with dataset ${dataSetRef2}`);
        await client.enableReporting(rcbRef2, dataSetRef2);

    } catch (err) {
        console.error('Error in handleConnectionOpened:', err.message);
    }
}*/

async function handleConnectionOpened() {    
    try {
        const dataModel = await client.browseDataModel();
        
        const dataSets = [];
        const reports = [];
        
        dataModel.forEach(ld => {
            console.log(`\nLogical Device: ${ld.name}`);
            
            ld.logicalNodes.forEach(ln => {
                console.log(`  Logical Node: ${ln.name} (${ln.reference})`);
                
                // Вывод DataSets (только то, что доступно в корневом обходе)
                if (ln.dataSets && ln.dataSets.length > 0) {
                    console.log(`    Datasets (${ln.dataSets.length}):`);
                    ln.dataSets.forEach(ds => {
                        console.log(`      - ${ds.reference}`);
                        dataSets.push(ds);
                    });
                }
                
                // Вывод отчетов (только то, что доступно в корневом обходе)
                if (ln.reports && ln.reports.length > 0) {
                    console.log(`    Reports (${ln.reports.length}):`);
                    ln.reports.forEach((report, index) => {
                        console.log(`      [${index + 1}] ${report.reference}`);
                        console.log(`          Type: ${report.type} (${report.description || 'N/A'})`);
                        reports.push(report);
                    });
                } else {
                    console.log(`    No reports found`);
                }
            });
        });

        console.log('\n=== SUMMARY ===');
        console.log(`Total Logical Devices: ${dataModel.length}`);
        
        let totalDataSets = 0;
        let totalReports = 0;
        
        dataModel.forEach(ld => {
            ld.logicalNodes.forEach(ln => {
                totalDataSets += (ln.dataSets ? ln.dataSets.length : 0);
                totalReports += (ln.reports ? ln.reports.length : 0);
            });
        });
        
        console.log(`Total Datasets found: ${totalDataSets}`);
        console.log(`Total Reports found: ${totalReports}`);
        
        // Выводим все найденные отчеты для удобства
        console.log('\n=== ALL FOUND REPORTS ===');
        reports.forEach((report, index) => {
            console.log(`${index + 1}. ${report.reference} (${report.type})`);
        });

        console.log('\n=== RECOMMENDED REPORTS FOR ENABLING ===');
        // Ищем отчеты с DataSet01 (как в примере)
        const recommendedReports = reports.filter(r => 
            r.reference.includes('ReportBlock0101')
        );
        
        if (recommendedReports.length > 0) {
            recommendedReports.forEach((report, index) => {
                console.log(`${index + 1}. ${report.reference}`);
                console.log(`   To enable: client.enableReporting("${report.reference}", "<datasetRef>")`);
                console.log(`   (Get datasetRef by calling browseDataModel("${report.reference}"))`);
            });
        } else {
            console.log('No reports with ReportBlock0101 found.');
            if (reports.length > 0) {
                console.log('\nAvailable reports:');
                reports.forEach((report, index) => {
                    console.log(`${index + 1}. ${report.reference} -> ${report.type}`);
                });
            }
        }
    } catch (err) {
        console.error('Error in handleConnectionOpened:', err.message);
    }
}

async function handleConnectionOpened2() {
    try {
        const dataModel = await client.browseDataModel();
        console.log('Data Model:', util.inspect(dataModel, { depth: null }));

        const dataSets = [];
        const reports = [];
        
        dataModel.forEach(ld => {
            console.log(`\nLogical Device: ${ld.name}`);
            
            ld.logicalNodes.forEach(ln => {
                console.log(`  Logical Node: ${ln.name}`);
                
                // Вывод DataSets
                if (ln.dataSets && ln.dataSets.length > 0) {
                    ln.dataSets.forEach(ds => {
                        console.log(`    Dataset: ${ds.reference} (Deletable: ${ds.isDeletable})`);
                        dataSets.push(ds);
                    });
                }
                
                // Вывод отчетов (RCB - Report Control Blocks)
                if (ln.reports && ln.reports.length > 0) {
                    console.log(`    Reports (${ln.reports.length}):`);
                    ln.reports.forEach((report, index) => {
                        console.log(`      [${index + 1}] ${report.reference}`);
                        console.log(`          Type: ${report.type} (${report.description})`);
                        if (report.datasetRef) {
                            console.log(`          Dataset: ${report.datasetRef}`);
                        }
                        if (report.reportId) {
                            console.log(`          Report ID: ${report.reportId}`);
                        }
                        console.log(`          Enabled: ${report.enabled}`);
                        reports.push(report);
                    });
                } else {
                    console.log(`    No reports found for ${ln.reference}`);
                }
            });
        });

        console.log('\n=== SUMMARY ===');
        console.log(`Total Logical Devices: ${dataModel.length}`);
        console.log(`Total Datasets found: ${dataSets.length}`);
        console.log(`Total Reports found: ${reports.length}`);
        
        // Выводим все найденные отчеты для удобства
        console.log('\n=== ALL FOUND REPORTS ===');
        reports.forEach((report, index) => {
            console.log(`${index + 1}. ${report.reference} (${report.type})`);
        });

        // 1. Читаем модель устройства
        /*const dataModel = await client.browseDataModel();
        console.log('Data Model:', util.inspect(dataModel, { depth: null }));

        const dataSets = [];
        dataModel.forEach(ld => {
            ld.logicalNodes.forEach(ln => {
                ln.dataSets.forEach(ds => {
                    console.log(`Found dataset: ${ds.reference}`);
                    dataSets.push(ds);
                });
            });
        });*/

        // 2. Читаем одиночные значения
        console.log('Reading single values...');
        const dataRefs = [
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/GGIO1.Ind1[ST]',
            'WAGO61850ServerDevice/CALH12.GrAlm.stVal'
        ];
        const readRefResult = await client.readData(dataRefs); 
        console.log("readRefResult " + util.inspect(readRefResult, { depth: null }));

        // 3. Читаем и кэшируем структуры (делаем это один раз)
        console.log('First read (with caching)...');
        const firstRead = await client.readDataSetModel([
            'WAGO61850ServerDevice/LLN0.DataSet01',
            'WAGO61850ServerDevice/LLN0.DataSet02',
            'WAGO61850ServerDevice/LLN0.DataSet03'
        ]);
        
        console.log('First read completed, structures cached');
        
        // 4. Теперь можем использовать быстрый polling значений Dataset
        console.log('\nStarting fast polling...');
        
        for (let i = 0; i < 10; i++) {
            console.log(`\n--- Poll ${i+1} ---`);
            
            const startTime = Date.now();
            
            const pollResults = await client.pollDataSetValues(['WAGO61850ServerDevice/LLN0.DataSet01']); // Быстрое чтение значений DataSet
            
            const endTime = Date.now();
            
            pollResults.forEach((result, idx) => {
                if (result.isValid) {
                    console.log(`DataSet ${result.datasetRef}: ${result.count} values`);
                    console.log(`  Read time: ${result.readTimeMicros} µs`);
                    console.log(`  Process time: ${result.processTimeMicros} µs`);
                    
                    // Выводим значения
                    Object.entries(result.values).forEach(([ref, value]) => {
                        console.log(`  ${ref}:`, util.inspect(value, { depth: null }));
                    });
                } else {
                    console.error(`Error: ${result.errorReason}`);
                }
            });
            
            console.log(`Total poll time: ${endTime - startTime} ms`);
            
            // Ждем перед следующим опросом
            await sleep(1000);
        }
        
    } catch (err) {
        console.error('Error in handleConnectionOpened2:', err.message);
    }
}

/*async function exploreModel() {
    try {
        console.log('\n=== 1. Получение корневых узлов ===');
        const rootNodes = await client.browseDataModel();
        
        console.log('\nНайдено Logical Nodes:');
        rootNodes.forEach((ln, index) => {
            console.log(`\n${index + 1}. ${ln.name} (${ln.reference})`);
            
            // Выводим ВСЕ датасеты
            if (ln.dataSets && ln.dataSets.length > 0) {
                console.log(`   Datasets (${ln.dataSets.length}):`);
                ln.dataSets.forEach((ds, idx) => {
                    console.log(`     ${idx + 1}. ${ds.name}: ${ds.reference}`);
                });
            } else {
                console.log(`   Datasets: 0`);
            }
            
            // Выводим ВСЕ отчеты
            if (ln.reports && ln.reports.length > 0) {
                console.log(`   Reports (${ln.reports.length}):`);
                ln.reports.forEach((report, idx) => {
                    const typeDesc = report.type === 'RP' ? 'Unbuffered' : 'Buffered';
                    console.log(`     ${idx + 1}. ${report.name} (${report.type} - ${typeDesc}): ${report.reference}`);
                });
            } else {
                console.log(`   Reports: 0`);
            }
        });
        
        // Выбираем LLN0 для дальнейшего исследования
        const lln0 = rootNodes.find(ln => ln.name === 'LLN0');
        if (lln0) {
            console.log('\n=== 2. Исследуем LLN0 ===');
            const lln0Details = await client.browseDataModel(lln0.reference);
            
            console.log(`\nDataObjects в ${lln0Details.reference}: ${llln0Details.dataObjectsCount}`);
            console.log(`DataSets в ${lln0Details.reference}: ${llln0Details.dataSetsCount}`);
            
            // Показываем ВСЕ DataObjects
            console.log('\nВсе DataObjects:');
            lln0Details.dataObjects.forEach((doObj, index) => {
                console.log(`${index + 1}. ${doObj.name} (${doObj.cdc || 'Unknown'}) - ${doObj.reference}`);
            });
            
            // Выбираем первый DataSet для кэширования
            if (lln0Details.dataSets.length > 0) {
                const firstDataSet = lln0Details.dataSets[0];
                console.log(`\n=== 3. Кэшируем DataSet ${firstDataSet.reference} ===`);
                const dsDetails = await client.browseDataModel(firstDataSet.reference);
                
                console.log(`DataSet ${dsDetails.reference}:`);
                console.log(`  Удаляемый: ${dsDetails.isDeletable}`);
                console.log(`  Членов: ${dsDetails.memberCount}`);
                console.log('\n  Первые члены:');
                dsDetails.members.slice(0, 5).forEach((member, index) => {
                    console.log(`  ${index + 1}. ${member.reference}`);
                });
                
                // Теперь можем быстро читать этот DataSet
                console.log('\n=== 4. Быстрое чтение DataSet ===');
                const pollResults = await client.pollDataSetValues([firstDataSet.reference]);
                console.log('Poll results:', util.inspect(pollResults, { depth: 2 }));
            }
            
            // Выбираем первый отчет для кэширования
            if (lln0.reports.length > 0) {
                const firstReport = lln0.reports.find(r => r.reference.includes('ReportBlock0101'));
                if (firstReport) {
                    console.log(`\n=== 5. Кэшируем отчет ${firstReport.reference} ===`);
                    const reportDetails = await client.browseDataModel(firstReport.reference);
                    
                    console.log(`Отчет ${reportDetails.reference}:`);
                    console.log(`  Тип: ${reportDetails.reportType}`);
                    console.log(`  DataSet: ${reportDetails.datasetRef}`);
                    console.log(`  Включен: ${reportDetails.enabled}`);
                    console.log(`  Report ID: ${reportDetails.reportId}`);
                    
                    // Подписываемся на отчет
                    console.log(`\n=== 6. Подписываемся на отчет ===`);
                    await client.enableReporting(firstReport.reference, reportDetails.datasetRef);
                }
            }
        }
        
        // Пример чтения одиночного значения
        console.log('\n=== 7. Чтение одиночных значений ===');
        const singleValues = await client.readData([
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/GGIO1.Ind1[ST]'
        ]);
        console.log('Single values:', util.inspect(singleValues, { depth: 2 }));
        
    } catch (err) {
        console.error('Error in exploreModel:', err.message);
    }
}*/

async function exploreModel() {
    try {
        console.log('\n=== 1. Получение корневых узлов ===');
        const rootNodes = await client.browseDataModel();  // теперь только name, reference, type

        console.log('\nНайдено Logical Nodes:');
        for (const ln of rootNodes) {
            console.log(`\n${ln.name} (${ln.reference})`);

            // Получаем детальную информацию о логическом узле
            const lnDetails = await client.browseDataModel(ln.reference);

            // Выводим DataSets
            if (lnDetails.dataSets && lnDetails.dataSets.length > 0) {
                console.log(`   Datasets (${lnDetails.dataSets.length}):`);
                lnDetails.dataSets.forEach((ds, idx) => {
                    console.log(`     ${idx + 1}. ${ds.name}: ${ds.reference}`);
                });
            } else {
                console.log(`   Datasets: 0`);
            }

            // Выводим Reports
            if (lnDetails.reports && lnDetails.reports.length > 0) {
                console.log(`   Reports (${lnDetails.reports.length}):`);
                lnDetails.reports.forEach((report, idx) => {
                    const typeDesc = report.type === 'RP' ? 'Unbuffered' : 'Buffered';
                    console.log(`     ${idx + 1}. ${report.name} (${report.type} - ${typeDesc}): ${report.reference}`);
                });
            } else {
                console.log(`   Reports: 0`);
            }
        }
        
        // Выбираем LLN0 для дальнейшего исследования
        const lln0 = rootNodes.find(ln => ln.name === 'LLN0');
        if (lln0) {
            console.log('\n=== 2. Исследуем LLN0 ===');
            const lln0Details = await client.browseDataModel(lln0.reference);
            
            console.log(`\nDataObjects в ${lln0Details.reference}: ${lln0Details.dataObjectsCount}`);
            console.log(`DataSets в ${lln0Details.reference}: ${lln0Details.dataSetsCount}`);
            
            // Показываем ВСЕ DataObjects
            console.log('\nВсе DataObjects:');
            lln0Details.dataObjects.forEach((doObj, index) => {
                console.log(`${index + 1}. ${doObj.name} (${doObj.cdc || 'Unknown'}) - ${doObj.reference}`);
            });
            
            // Выбираем первый DataSet для кэширования
            // Выбираем первый DataSet для кэширования
            if (lln0Details.dataSets.length > 0) {
                const firstDataSet = lln0Details.dataSets[0];
                console.log(`\n=== 3. Кэшируем DataSet ${firstDataSet.reference} ===`);
                
                // ✅ ПРАВИЛЬНО: вызываем readDataSetModel для заполнения кэша структур
                await client.readDataSetModel([firstDataSet.reference]);
                
                // Теперь можно быстро читать этот DataSet
                console.log('\n=== 4. Быстрое чтение DataSet ===');
                const pollResults = await client.pollDataSetValues([firstDataSet.reference]);         
                
                console.log('\nPoll results:');
                pollResults.forEach((result, idx) => {
                    if (result.isValid) {
                        console.log(`\nDataSet ${result.datasetRef}: ${result.count} значений`);
                        console.log(`  Read time: ${result.readTimeMicros} µs`);
                        console.log(`  Process time: ${result.processTimeMicros} µs`);
                        
                        // Выводим ВСЕ значения
                        console.log('\n  Значения:');
                        Object.entries(result.values).forEach(([ref, value], index) => {
                            console.log(`  [${index + 1}] ${ref}:`, util.inspect(value, { 
                                depth: null, 
                                colors: true,
                                maxArrayLength: 10
                            }));
                        });
                    } else {
                        console.error(`  Error: ${result.errorReason}`);
                    }
                });
            }
            
            // Выбираем первый отчет для кэширования
            if (lln0.reports.length > 0) {
                const firstReport = lln0.reports.find(r => r.reference.includes('ReportBlock0101'));
                if (firstReport) {
                    console.log(`\n=== 5. Кэшируем отчет ${firstReport.reference} ===`);
                    const reportDetails = await client.browseDataModel(firstReport.reference);
                    
                    console.log(`\nОтчет ${reportDetails.reference}:`);
                    console.log(`  Тип: ${reportDetails.reportType}`);
                    console.log(`  DataSet: ${reportDetails.datasetRef}`);
                    console.log(`  Включен: ${reportDetails.enabled}`);
                    console.log(`  Report ID: ${reportDetails.reportId || 'N/A'}`);
                    console.log(`  Triggers: ${reportDetails.trgOps}`);
                    console.log(`  Integrity Period: ${reportDetails.intgPd} ms`);
                    console.log(`  Buffer Time: ${reportDetails.bufTm} ms`);
                    console.log(`  GI: ${reportDetails.gi}`);
                    
                    // Подписываемся на отчет
                    console.log(`\n=== 6. Подписываемся на отчет ===`);
                    await client.enableReporting(firstReport.reference, reportDetails.datasetRef);
                }
            }
        }
        
        // Пример чтения одиночного значения
        console.log('\n=== 7. Чтение одиночных значений ===');
        const singleValues = await client.readData([
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/GGIO1.Ind1[ST]'
        ]);
        
        console.log('\nSingle values:');
        singleValues.forEach((result, index) => {
            if (result.isValid) {
                console.log(`[${index + 1}] ${result.dataRef}:`, util.inspect(result.value, { 
                    depth: null, 
                    colors: true 
                }));
            } else {
                console.log(`[${index + 1}] ${result.dataRef}: ERROR - ${result.errorReason}`);
            }
        });
        
    } catch (err) {
        console.error('Error in exploreModel:', err.message);
        console.error(err.stack);
    }
}

async function main() {
    try {
        console.log('Starting client...');
        await client.connect({
            ip: '192.168.0.106',
            port: 102,
            clientID: 'mms_client1',
            reconnectDelay: 2,
            //heartbeatInterval: 3000 //новый параметр - интервал эхо-запросов для поддержки соединения
        });

        await sleep(5000);

        console.log('Waiting for data and reports...');
        await sleep(30000);

        const rcbRef2 = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0101';
        console.log(`Disabling reporting for ${rcbRef2}`);
        await client.disableReporting(rcbRef2);

        console.log('Client status:', client.getStatus());
        console.log('Closing client...');
        await client.close();
        console.log('Client closed.');

    } catch (err) {
        console.error('Main error:', err.message);
        await client.close().catch(e => console.error('Close error:', e.message));
    }
}

main().catch(err => console.error('Fatal error:', err.message));