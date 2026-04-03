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
        logWithTime('Connection opened, starting concurrent tasks...');
        runConcurrentTasks().catch(err => logWithTime('Error in concurrent tasks:', err));
    }

    if (event === 'data' && data.type === 'error') {
        logWithTime(`!!! ERROR: ${data.reason}`);
    }

    if (event === 'conn' && data.event === 'stateChanged') {
        logWithTime(`Connection state changed: ${data.state}, isConnected: ${data.isConnected}`);
    }
});

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function runConcurrentTasks() {
    try {
        // 1. Получаем модель данных (один раз для поиска ресурсов)
        logWithTime('Step 1: Browsing data model to find dataset and report...');
        const browseStart = now();
        const dataModel = await client.browseDataModel();
        logWithTime(`Initial browse completed in ${now() - browseStart} ms`);

        // 2. Ищем подходящие DataSet и отчет
        let targetDataset = null;
        let targetReport = null;

        for (const ln of dataModel) {
            if (ln.dataSets && ln.dataSets.length > 0 && !targetDataset) {
                targetDataset = ln.dataSets[0];
                logWithTime(`Found dataset: ${targetDataset.reference}`);
            }
            if (ln.reports && ln.reports.length > 0 && !targetReport) {
                // Берем первый отчет (позже уточним, ссылается ли он на наш dataset)
                targetReport = ln.reports[0];
                logWithTime(`Found potential report: ${targetReport.reference}`);
            }
            if (targetDataset && targetReport) break;
        }

        if (!targetDataset) {
            logWithTime('No datasets found, cannot proceed.');
            return;
        }

        // Уточняем детали отчета (чтобы получить datasetRef)
        let reportDetails = null;
        if (targetReport) {
            try {
                reportDetails = await client.browseDataModel(targetReport.reference);
                logWithTime(`Report details: datasetRef = ${reportDetails.datasetRef || 'none'}`);
            } catch (err) {
                logWithTime(`Failed to get report details: ${err.message}`);
            }
        }

        // Если отчет не ссылается на наш dataset, ищем другой
        if (!reportDetails || reportDetails.datasetRef !== targetDataset.reference) {
            logWithTime('Initial report does not match dataset, searching for matching report...');
            let found = false;
            for (const ln of dataModel) {
                if (!ln.reports) continue;
                for (const rpt of ln.reports) {
                    try {
                        const details = await client.browseDataModel(rpt.reference);
                        if (details.datasetRef === targetDataset.reference) {
                            targetReport = rpt;
                            reportDetails = details;
                            found = true;
                            logWithTime(`Found matching report: ${targetReport.reference} (dataset: ${details.datasetRef})`);
                            break;
                        }
                    } catch (err) {
                        // ignore
                    }
                }
                if (found) break;
            }
            if (!found) {
                logWithTime('No report matching the dataset found, will continue without reporting.');
                targetReport = null;
            }
        }

        // 3. Кэшируем DataSet (readDataSetModel)
        logWithTime(`Step 2: Caching dataset ${targetDataset.reference}...`);
        const cacheStart = now();
        await client.readDataSetModel([targetDataset.reference]);
        logWithTime(`Caching completed in ${now() - cacheStart} ms`);

        // 4. Включаем отчет, если найден
        if (targetReport && reportDetails && reportDetails.datasetRef) {
            logWithTime(`Step 3: Enabling report ${targetReport.reference} with dataset ${reportDetails.datasetRef}...`);
            const enableStart = now();
            await client.enableReporting(targetReport.reference, reportDetails.datasetRef);
            logWithTime(`Reporting enabled in ${now() - enableStart} ms`);
        } else {
            logWithTime('No suitable report found, skipping report enabling.');
        }

        // 5. Запускаем параллельные задачи:
        //    - Поллинг каждые 2 секунды
        //    - Сканирование модели каждые 5 секунд
        //    - Основной цикл на 30 секунд
        logWithTime('Step 4: Starting concurrent tasks (polling every 2s, browsing every 5s) for 30 seconds...');

        let stop = false;
        let pollCount = 0;
        let browseCount = 0;

        // Задача поллинга
        const pollTask = (async () => {
            while (!stop) {
                const start = now();
                try {
                    const results = await client.pollDataSetValues([targetDataset.reference]);
                    const duration = now() - start;
                    if (results && results[0]) {
                        const res = results[0];
                        if (res.isValid) {
                            logWithTime(`[POLL #${++pollCount}] duration=${duration}ms, readTime=${res.readTimeMicros}µs, processTime=${res.processTimeMicros}µs, values=${res.count}`);
                        } else {
                            logWithTime(`[POLL #${++pollCount}] ERROR: ${res.errorReason}`);
                        }
                    } else {
                        logWithTime(`[POLL #${++pollCount}] no results`);
                    }
                } catch (err) {
                    logWithTime(`[POLL #${++pollCount}] exception: ${err.message}`);
                }
                await sleep(2000);
            }
        })();

        // Задача сканирования модели
        const browseTask = (async () => {
            while (!stop) {
                const start = now();
                try {
                    await client.browseDataModel();
                    const duration = now() - start;
                    logWithTime(`[BROWSE #${++browseCount}] completed in ${duration}ms`);
                } catch (err) {
                    logWithTime(`[BROWSE #${++browseCount}] error: ${err.message}`);
                }
                await sleep(5000);
            }
        })();

        // Ждём 60 секунд
        await sleep(60000);
        stop = true;

        // Дожидаемся завершения задач (с таймаутом)
        await Promise.race([pollTask, browseTask]);
        logWithTime('Concurrent tasks stopped.');

        // 6. Отключаем отчет, если был включен
        if (targetReport) {
            logWithTime('Step 5: Disabling report...');
            const disableStart = now();
            await client.disableReporting(targetReport.reference);
            logWithTime(`Report disabled in ${now() - disableStart} ms`);
        }

        logWithTime('All tasks completed. Closing client...');
        await client.close();
        logWithTime('Client closed.');

    } catch (err) {
        logWithTime(`Fatal error: ${err.stack}`);
        await client.close().catch(e => logWithTime(`Close error: ${e.message}`));
    }
}

// Подключение
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