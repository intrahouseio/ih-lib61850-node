const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

const client = new MmsClient((event, data) => {
    console.log(`Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    if (event === 'conn' && data.event === 'opened') {
        console.log('Connection opened, browsing data model...');
        handleConnectionOpened();
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

async function handleConnectionOpened() {
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

        //для теста
        const dataModel2 = await client.TestDebug();

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
            const printValue = (value, indent = '  ') => {
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
        });

        console.log('Reading data...');
        const dataRefs = [
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/GGIO1.Ind1.stVal',
            'WAGO61850ServerDevice/CALH12.GrAlm.stVal'
        ];
        const readRefResult = await client.readData(dataRefs); 
        console.log("readRefResult " + util.inspect(readRefResult, { depth: null }));

        /*const rcbRef = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0101';
        const dataSetRef = 'WAGO61850ServerDevice/LLN0.DataSet01';
        console.log(`Enabling reporting for ${rcbRef} with dataset ${dataSetRef}`);
        await client.enableReporting(rcbRef, dataSetRef);*/

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
        console.log("readRefResult " + util.inspect(readRefResult, { depth: null }));

        const rcbRef = 'A01LD0/LLN0.RP.repTI1';
        const dataSetRef = 'A01LD0/LLN0.TI_ASU';
        console.log(`Enabling reporting for ${rcbRef} with dataset ${dataSetRef}`);
        await client.enableReporting(rcbRef, dataSetRef);

        const rcbRef2 = 'A01LD0/LLN0.BR.repTS1';
        const dataSetRef2 = 'A01LD0/LLN0.TS_ASU';
        console.log(`Enabling reporting for ${rcbRef2} with dataset ${dataSetRef2}`);
        await client.enableReporting(rcbRef2, dataSetRef2);*/

    } catch (err) {
        console.error('Error in handleConnectionOpened:', err.message);
    }
}

async function main() {
    try {
        console.log('Starting client...');
        await client.connect({
            ip: '192.168.0.142',
            port: 102,
            clientID: 'mms_client1',
            reconnectDelay: 2
        });

        await sleep(5000);

        console.log('Waiting for data and reports...');
        await sleep(15000);

        /*const rcbRef = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0101';
        console.log(`Disabling reporting for ${rcbRef}`);
        await client.disableReporting(rcbRef);*/

        const rcbRef2 = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0201';
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