/*const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

function readDataSet(client, ref) {
    return new Promise((resolve, reject) => {
        const onData = (data) => {
            if (data.type === 'data' && data.event === 'dataSet' && data.dataSetRef === ref) {
                cleanup();
                resolve(data.value);
            } else if (data.type === 'error' && data.dataSetRef === ref) {
                cleanup();
                reject(new Error(data.reason));
            }
        };

        const cleanup = () => {
            client.off('data', onData);
            clearTimeout(timeoutId);
        };

        const timeoutId = setTimeout(() => {
            cleanup();
            reject(new Error(`Timeout waiting for dataset ${ref}`));
        }, 5000);

        client.on('data', onData);
        client.readDataSetValues(ref);
    });
}

const client = new MmsClient((event, data) => {
    console.log(`Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    if (event === 'conn' && data.event === 'opened') {
        console.log('Connection opened, browsing data model...');
        client.browseDataModel()
            .then(async dataModel => {
                console.log('Data Model:', util.inspect(dataModel, { depth: null }));

                // Extract dataset references
                const dataSets = [];
                dataModel.forEach(ld => {
                    ld.logicalNodes.forEach(ln => {
                        ln.dataSets.forEach(ds => {
                            console.log(`Found dataset: ${ds.reference}`);
                            dataSets.push(ds);
                        });
                    });
                });

                // Read datasets one by one
                console.log('Available datasets from browseDataModel:');
                for (const ds of dataSets) {
                    console.log(`- ${ds.reference}`);
                    try {
                        const values = await readDataSet(client, ds.reference);
                        console.log(`Values for ${ds.reference}:`, util.inspect(values, { depth: null }));
                    } catch (err) {
                        console.error(`Error reading ${ds.reference}:`, err.message);
                    }
                }

                // Enable reporting
                const rcbRef = 'simpleIOGenericIO/LLN0.RP.EventsRCB01';
                const dataSetRef = 'simpleIOGenericIO/LLN0.Events';
                console.log(`Enabling reporting for ${rcbRef} with dataset ${dataSetRef}`);
                client.enableReporting(rcbRef, dataSetRef);

                // Wait for some reports
                await new Promise(resolve => setTimeout(resolve, 10000));

                // Disable reporting
                console.log(`Disabling reporting for ${rcbRef}`);
                client.disableReporting(rcbRef);
            })
            .catch(err => console.error('Error browsing data model:', err.message));

        console.log('Reading data...');
        const dataRefs = [
            'simpleIOGenericIO/GGIO1.AnIn1.mag.f',
            'simpleIOGenericIO/GGIO1.AnIn2.mag.f',
            'simpleIOGenericIO/GGIO1.AnIn3.mag.f',
            'simpleIOGenericIO/GGIO1.AnIn4.mag.f',
            'simpleIOGenericIO/GGIO1.SPCSO1.stVal',
            'simpleIOGenericIO/GGIO1.SPCSO2.stVal',
            'simpleIOGenericIO/GGIO1.SPCSO3.stVal',
            'simpleIOGenericIO/GGIO1.SPCSO4.stVal'
        ];
        dataRefs.forEach(ref => client.readData(ref));

        console.log('And now we try do control operation!!!!!!!!!!!!!!!...');
        client.controlObject("simpleIOGenericIO/GGIO1.SPCSO1", true);
    }

    if (event === 'data' && data.type === 'data') {
        if (data.event === 'logicalDevices') {
            console.log(`Logical Devices received: ${util.inspect(data.logicalDevices, { depth: null })}`);
        } else if (data.event === 'dataSetDirectory') {
            console.log(`DataSet Directory for ${data.logicalNodeRef}: ${util.inspect(data.dataSets, { depth: null })}`);
        } else if (data.event === 'dataModel') {
            console.log(`Data Model received: ${util.inspect(data.dataModel, { depth: null })}`);
        } else if (data.event === 'dataSet') {
            console.log(`DataSet received for ${data.dataSetRef}: ${util.inspect(data.value, { depth: null })}`);
        } else if (data.event === 'report') {
            console.log(`Report received for ${data.rcbRef} (rptId: ${data.rptId}):`);
            if (data.timestamp) {
                console.log(`  Timestamp: ${data.timestamp}`);
            }
            data.values.forEach((value, index) => {
                if (data.reasonsForInclusion[index] !== 0) {
                    console.log(`  Value[${index}]: ${util.inspect(value, { depth: null })}, Reason: ${data.reasonsForInclusion[index]}`);
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
        }
    }
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    try {
        console.log('Starting client...');
        await client.connect({
            ip: '127.0.0.1',
            port: 102,
            clientID: 'mms_client1',
            reconnectDelay: 2
        });

        await sleep(20000);

        console.log('Client status:', client.getStatus());

        console.log('Closing client...');
        await client.close();
        console.log('Client closed.');
    } catch (err) {
        console.error('Main error:', err.message);
        await client.close().catch(e => console.error('Close error:', e.message));
    }
}

main().catch(err => console.error('Fatal error:', err.message));*/

const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

const client = new MmsClient((event, data) => {
    console.log(`Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    if (event === 'conn' && data.event === 'opened') {
        console.log('Connection opened, browsing data model...');
        client.browseDataModel()
            .then(dataModel => {
                console.log('Data Model:', util.inspect(dataModel, { depth: null }));

                // Извлечение ссылок на датасеты
                const dataSets = [];
                dataModel.forEach(ld => {
                    ld.logicalNodes.forEach(ln => {
                        ln.dataSets.forEach(ds => {
                            console.log(`Found dataset: ${ds.reference}`);
                            dataSets.push(ds);
                        });
                    });
                });

                // Чтение датасетов напрямую без промисов
                console.log('Reading datasets...');
                dataSets.forEach(ds => {
                    console.log(`- ${ds.reference}`);
                    client.readDataSetValues(ds.reference);
                });

                // Чтение данных
                console.log('Reading data...');
                const dataRefs = [
                    'simpleIOGenericIO/GGIO1.AnIn1.mag.f',
                    'simpleIOGenericIO/GGIO1.AnIn2.mag.f',
                    'simpleIOGenericIO/GGIO1.AnIn3.mag.f',
                    'simpleIOGenericIO/GGIO1.AnIn4.mag.f',
                    'simpleIOGenericIO/GGIO1.SPCSO1.stVal',
                    'simpleIOGenericIO/GGIO1.SPCSO2.stVal',
                    'simpleIOGenericIO/GGIO1.SPCSO3.stVal',
                    'simpleIOGenericIO/GGIO1.SPCSO4.stVal'
                ];
                dataRefs.forEach(ref => client.readData(ref));

                // Включение отчётов
                const rcbRef = 'simpleIOGenericIO/LLN0.RP.EventsRCB01';
                const dataSetRef = 'simpleIOGenericIO/LLN0.Events';
                console.log(`Enabling reporting for ${rcbRef} with dataset ${dataSetRef}`);
                client.enableReporting(rcbRef, dataSetRef);
            })
            .catch(err => console.error('Error browsing data model:', err.message));
    }

    if (event === 'data' && data.type === 'data') {
        if (data.event === 'logicalDevices') {
            console.log(`Logical Devices received: ${util.inspect(data.logicalDevices, { depth: null })}`);
        } else if (data.event === 'dataSetDirectory') {
            console.log(`DataSet Directory for ${data.logicalNodeRef}: ${util.inspect(data.dataSets, { depth: null })}`);
        } else if (data.event === 'dataModel') {
            console.log(`Data Model received: ${util.inspect(data.dataModel, { depth: null })}`);
        } else if (data.event === 'dataSet') {
            console.log(`DataSet received for ${data.dataSetRef}: ${util.inspect(data.value, { depth: null })}`);
        } else if (data.event === 'report') {
            console.log(`Report received for ${data.rcbRef} (rptId: ${data.rptId}):`);
            if (data.timestamp) {
                console.log(`  Timestamp: ${data.timestamp}`);
            }
            data.values.forEach((value, index) => {
                if (data.reasonsForInclusion[index] !== 0) {
                    console.log(`  Value[${index}]: ${util.inspect(value, { depth: null })}, Reason: ${data.reasonsForInclusion[index]}`);
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
        }
    }
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    try {
        console.log('Starting client...');
        await client.connect({
            ip: '127.0.0.1',
            port: 102,
            clientID: 'mms_client1',
            reconnectDelay: 2
        });

        // Ожидание обработки данных и отчетов
        console.log('Waiting for data and reports...');
        await sleep(10000);

        // Отключение отчетов
        const rcbRef = 'simpleIOGenericIO/LLN0.RP.EventsRCB01';
        console.log(`Disabling reporting for ${rcbRef}`);
        client.disableReporting(rcbRef);

        // Выполнение операции управления
        console.log('And now we try do control operation!!!!!!!!!!!!!!!...');
        client.controlObject("simpleIOGenericIO/GGIO1.SPCSO1", true);

        // Дополнительное ожидание для обработки операции управления
        await sleep(5000);

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