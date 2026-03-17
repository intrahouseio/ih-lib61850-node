const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

// Вспомогательные функции
const now = () => Date.now();

function logWithTime(...args) {
    const timestamp = new Date().toISOString();
    console.log(`[${timestamp}]`, ...args);
}

const client = new MmsClient((event, data) => {
    const eventTime = now();
    logWithTime(`Event: ${event}, Data type: ${data.type || 'N/A'}, event: ${data.event || 'N/A'}`);

    if (event === 'data' && data.event === 'report') {
        logWithTime(`>>> REPORT RECEIVED for ${data.rcbRef}, timestamp: ${data.timestamp || 'none'}`);
        if (data.values) {
            const refs = Object.keys(data.values);
            logWithTime(`    Report contains ${refs.length} elements`);
            refs.slice(0, 3).forEach(ref => {
                logWithTime(`    ${ref}: ${util.inspect(data.values[ref], { depth: 1 })}`);
            });
        }
    }

    if (event === 'data' && data.event === 'multipleDataSets') {
        logWithTime(`>>> POLL RESULT for ${data.datasetRefs}`);
    }

    if (event === 'conn' && data.event === 'opened') {
        logWithTime('Connection opened, starting diagnostics...');
        runDiagnostics().catch(err => logWithTime('Diagnostics error:', err));
    }

    if (event === 'data' && data.type === 'error') {
        logWithTime(`!!! ERROR: ${data.reason}`);
    }

    if (event === 'conn' && data.event === 'stateChanged') {
        logWithTime(`Connection state changed: ${data.state}, isConnected: ${data.isConnected}`);
    }
});

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function runDiagnostics() {
    try {
        // 1. Получаем модель данных
        logWithTime('Step 1: Browsing data model...');
        const browseStart = now();
        const dataModel = await client.browseDataModel();
        logWithTime(`Browse completed in ${now() - browseStart} ms`);

        // Диагностический вывод структуры
        logWithTime('Data model structure (first 2 nodes):', 
            util.inspect(dataModel.slice(0, 2), { depth: 4, colors: true }));

        // 2. Ищем подходящие DataSet и отчеты
        let targetDataset = null;
        let targetReport = null;

        // Сначала ищем DataSet (берём первый)
        for (const ln of dataModel) {
            if (ln.dataSets && ln.dataSets.length > 0) {
                targetDataset = ln.dataSets[0];
                logWithTime(`Found dataset: ${targetDataset.reference}`);
                break;
            }
        }

        if (!targetDataset) {
            logWithTime('No datasets found, cannot proceed.');
            return;
        }

        // Теперь ищем отчёт, который ссылается на этот DataSet
        for (const ln of dataModel) {
            if (!ln.reports || ln.reports.length === 0) continue;
            
            for (const report of ln.reports) {
                logWithTime(`Getting details for report ${report.reference}...`);
                try {
                    const details = await client.browseDataModel(report.reference);
                    if (details.datasetRef === targetDataset.reference) {
                        targetReport = details;
                        logWithTime(`Found matching report: ${report.reference} (dataset: ${details.datasetRef})`);
                        break;
                    }
                } catch (err) {
                    logWithTime(`Error getting details for report ${report.reference}: ${err.message}`);
                }
            }
            if (targetReport) break;
        }

        // Если не нашли подходящий, возьмём любой отчёт с datasetRef
        if (!targetReport) {
            for (const ln of dataModel) {
                if (!ln.reports || ln.reports.length === 0) continue;
                for (const report of ln.reports) {
                    try {
                        const details = await client.browseDataModel(report.reference);
                        if (details.datasetRef) {
                            targetReport = details;
                            logWithTime(`Found any report with dataset: ${report.reference} (dataset: ${details.datasetRef})`);
                            break;
                        }
                    } catch (err) {
                        logWithTime(`Error getting details for report ${report.reference}: ${err.message}`);
                    }
                }
                if (targetReport) break;
            }
        }

        // 3. Первичное чтение и кэширование DataSet (readDataSetModel)
        logWithTime(`Step 2: Caching dataset ${targetDataset.reference}...`);
        const cacheStart = now();
        await client.readDataSetModel([targetDataset.reference]);
        logWithTime(`Caching completed in ${now() - cacheStart} ms`);

        // 4. Включаем отчёт, если найден
        if (targetReport) {
            logWithTime(`Step 3: Enabling report ${targetReport.reference} with dataset ${targetReport.datasetRef}...`);
            const enableStart = now();
            await client.enableReporting(targetReport.reference, targetReport.datasetRef);
            logWithTime(`Reporting enabled in ${now() - enableStart} ms`);
        } else {
            logWithTime('No suitable report found, skipping report enabling.');
        }

        // 5. Запускаем цикл поллинга (10 итераций с интервалом 2 сек)
        logWithTime('Step 4: Starting polling loop (10 iterations, 2 sec interval)...');
        for (let i = 0; i < 10; i++) {
            logWithTime(`--- Poll iteration ${i+1} ---`);

            const pollStart = now();
            try {
                const pollResults = await client.pollDataSetValues([targetDataset.reference]);
                const pollEnd = now();
                logWithTime(`Poll completed in ${pollEnd - pollStart} ms`);

                if (pollResults && pollResults[0]) {
                    const res = pollResults[0];
                    if (res.isValid) {
                        logWithTime(`  Read time (µs): ${res.readTimeMicros}, Process time (µs): ${res.processTimeMicros}`);
                        logWithTime(`  Values count: ${res.count}`);
                        // Выводим первые 3 значения для проверки
                        const entries = Object.entries(res.values).slice(0, 3);
                        entries.forEach(([ref, val]) => {
                            logWithTime(`    ${ref}: ${util.inspect(val, { depth: 2 })}`);
                        });
                    } else {
                        logWithTime(`  Poll error: ${res.errorReason}`);
                    }
                }
            } catch (err) {
                logWithTime(`Poll exception: ${err.message}`);
            }

            await sleep(2000);
        }

        // 6. Отключаем отчёт
        if (targetReport) {
            logWithTime('Step 5: Disabling report...');
            const disableStart = now();
            await client.disableReporting(targetReport.reference);
            logWithTime(`Report disabled in ${now() - disableStart} ms`);
        }

        logWithTime('Diagnostics completed. Closing client...');
        await client.close();
        logWithTime('Client closed.');

    } catch (err) {
        logWithTime(`Fatal error in diagnostics: ${err.stack}`);
    }
}

// Подключаемся к устройству (без .catch, так как connect не возвращает Promise)
logWithTime('Starting client...');
try {
    client.connect({
        ip: '192.168.0.106',
        port: 102,
        clientID: 'mms_client1',
        reconnectDelay: 2
    });
} catch (err) {
    logWithTime('Connection error:', err);
}