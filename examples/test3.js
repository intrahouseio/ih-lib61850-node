// test_cdc.js
const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

const client = new MmsClient((event, data) => {
    if (event === 'conn' && data.event === 'opened') {
        console.log('✅ Connected');
        testCDC();
    }
    if (event === 'conn' && data.event === 'stateChanged' && data.state === 'closed') {
        console.log('🔌 Connection closed');
    }
    if (event === 'data' && data.type === 'error') {
        console.error('❌ Error:', data.reason);
    }
});

async function testCDC() {
    try {
        // Получаем корневые узлы (Logical Nodes)
        console.log('\n📡 Получение списка логических узлов...');
        const rootNodes = await client.browseDataModel();  // массив ln с именами, reference, dataSets, reports
        
        console.log(`\n🔍 Найдено логических узлов: ${rootNodes.length}\n`);
        
        // Перебираем все логические узлы и для каждого получаем DataObjects с CDC
        for (const ln of rootNodes) {
            console.log(`\n📦 Logical Node: ${ln.name} (${ln.reference})`);
            
            // Получаем DataObjects для данного LN
            const dataObjects = await client.browseDataModel(ln.reference);
            if (dataObjects && dataObjects.length > 0) {
                console.log(`   DataObjects (${dataObjects.length}):`);
                dataObjects.forEach(doObj => {
                    // Выводим имя, CDC и ссылку
                    console.log(`     - ${doObj.name} : ${doObj.cdc || 'Unknown'} -> ${doObj.reference}`);
                });
            } else {
                console.log('   DataObjects: none');
            }
        }
        
        // Альтернативно: можно выбрать конкретный LN для детального просмотра
        // Например, если есть LLN0:
        const lln0 = rootNodes.find(ln => ln.name === 'LLN0');
        if (lln0) {
            console.log(`\n⭐ Детальный вывод для LLN0:`);
            const lln0dos = await client.browseDataModel(lln0.reference);
            lln0dos.forEach(doObj => {
                console.log(`   ${doObj.name} (${doObj.cdc}) -> ${doObj.reference}`);
            });
        }
        
    } catch (err) {
        console.error('Ошибка в testCDC:', err);
    } finally {
        await client.close();
    }
}

// Подключение
client.connect({
    ip: '192.168.0.106',   // замените на IP вашего IED
    port: 102,
    clientID: 'test_cdc'
});