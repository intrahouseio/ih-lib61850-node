const { MmsClient } = require('../build/Release/addon_iec61850');
const util = require('util');

const client = new MmsClient((event, data) => {
    console.log(`Event: ${event}, Data:`, util.inspect(data, { depth: 2 }));
    if (event === 'conn' && data.event === 'opened') {
        console.log('✅ Connected');
        testDataSetFormat();
    }
    if (event === 'conn' && data.event === 'stateChanged' && data.state === 'closed') {
        console.log('🔌 Connection closed');
    }
});

async function testDataSetFormat() {
    try {
        // Замените на существующий DataSet из вашего устройства
        const datasetRef = 'WAGO61850ServerDevice/LLN0.DataSet02'; // или 'WAGO61850ServerDevice/LLN0.DataSet01'

        console.log(`\n📊 Получение информации о DataSet: ${datasetRef}`);
        const dsInfo = await client.browseDataModel(datasetRef);

        if (!dsInfo.isValid) {
            console.error('DataSet невалиден:', dsInfo.errorReason);
            return;
        }

        console.log(`\n✅ DataSet: ${dsInfo.reference}`);
        console.log(`   Членов: ${dsInfo.memberCount}\n`);

        const membersToShow = dsInfo.members.slice(0, 5);
        membersToShow.forEach((member, idx) => {
            console.log(`   ${idx + 1}. ${JSON.stringify(member, null, 2)}`);
        });

        if (dsInfo.memberCount > 5) {
            console.log(`   ... и ещё ${dsInfo.memberCount - 5} членов`);
        }

        const hasAllFields = dsInfo.members.every(m =>
            m.hasOwnProperty('reference') &&
            m.hasOwnProperty('name') &&
            m.hasOwnProperty('fc') &&
            m.hasOwnProperty('type')
        );
        console.log(`\n🔍 Все члены содержат нужные поля: ${hasAllFields ? '✅ ДА' : '❌ НЕТ'}`);

        const expectedFormat = {
            reference: 'A01LD0/MT_MMXU1.Hz',
            name: 'MT_MMXU1.Hz',
            fc: '[MX]',
            type: 'MV'
        };
        console.log('\n📝 Ожидаемый формат (пример):');
        console.log(JSON.stringify(expectedFormat, null, 2));

    } catch (err) {
        console.error('Ошибка:', err);
    } finally {
        await client.close();
    }
}

// Подключение (без await, так как connect не возвращает Promise)
client.connect({
    ip: '192.168.0.106',
    port: 102,
    clientID: 'test_dataset_format'
});