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
                    'WAGO61850ServerDevice/XCBR1.Pos.stVal',
                    'WAGO61850ServerDevice/GGIO1.Ind.stVal',
                    'WAGO61850ServerDevice/CALH1.GrAlm.stVal'                    
                ];
                dataRefs.forEach(ref => client.readData(ref));               

                // Включение отчётов
                const rcbRef = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0101';
                const dataSetRef = 'WAGO61850ServerDevice/LLN0.DataSet01';
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
            ip: '192.168.0.102',
            port: 102,
            clientID: 'mms_client1',
            reconnectDelay: 2
        });
        await sleep(5000);
        // Выполнение операции управления
        //console.log('And now we try do control operation!!!!!!!!!!!!!!!...');
        //client.controlObject("WAGO61850ServerDevice/XCBR1.Pos", true);

        // Дополнительное ожидание для обработки операции управления
        //await sleep(5000);

        // Ожидание обработки данных и отчетов
        console.log('Waiting for data and reports...');
        await sleep(30000);

        // Отключение отчетов
        const rcbRef = 'WAGO61850ServerDevice/LLN0.RP.ReportBlock0101';
        console.log(`Disabling reporting for ${rcbRef}`);
        client.disableReporting(rcbRef);        

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