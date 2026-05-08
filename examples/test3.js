const { MmsClient } = require('../build/Release/addon_iec61850');

const client = new MmsClient((event, data) => {
    // Обработчик событий – реагируем только на успешное открытие соединения
    if (event === 'conn' && data.event === 'opened') {
        console.log('Connected, browsing data model...');
        browseModel();
    }
});



async function browseModel() {
    try {
        const model = await client.browseDataModel();
        // Выводим первые 5 элементов модели для краткости (или весь объект)
        console.log('Data model (first 5 nodes):');
        console.log(JSON.stringify(model.slice(0, 5), null, 2));
        // Если нужно всё: console.log(JSON.stringify(model, null, 2));
        // Пример: кэшируем структуры для нескольких объектов
        await client.readDataModel([
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/LLN0.Mod[CO]',
            'WAGO61850ServerDevice/GGIO1.Ind1[ST]',
            'WAGO61850ServerDevice/LLN0.NamPlt[DC]',
            'WAGO61850ServerDevice/LLN0.Diag[CO]'
        ]);

        // Теперь последующие вызовы readData вернут осмысленные имена
        const values = await client.readData([
            'WAGO61850ServerDevice/XCBR1.Pos[ST]',
            'WAGO61850ServerDevice/LLN0.Mod[CO]',
            'WAGO61850ServerDevice/LLN0.NamPlt[DC]',
            'WAGO61850ServerDevice/LLN0.Diag[CO]'
        ]);
        console.log(values); // stVal, ctlVal и т.д.
    } catch (err) {
        console.error('Browse error:', err);
    } finally {
        await client.close();
        console.log('Client closed.');
    }
}

// Подключение к серверу (при необходимости измените IP и порт)
client.connect({
    ip: '192.168.0.106',
    port: 102,
    clientID: 'test_checker',
    reconnectDelay: 2
});