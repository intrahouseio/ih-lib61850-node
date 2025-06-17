const { NodeGOOSESubscriber } = require('../build/Release/addon_iec61850');
const subscriber = new NodeGOOSESubscriber((event, data) => {
    if (event === 'data' && data.type === 'data' && data.event === 'goose') {
        console.log(`GOOSE-сообщение для ${data.goCbRef}: ${JSON.stringify(data.values)}`);
    }
});
subscriber.subscribe('en5', 'WAGO61850ServerDevice/LLN0$GO$GSB_0');