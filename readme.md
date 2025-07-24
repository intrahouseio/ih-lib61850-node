# ih-lib61850-node

A cross-platform Node.js native addon for the **IEC 61850 protocol**, enabling seamless communication with substation automation systems. Built with `node-gyp` and `prebuild`, this addon ensures compatibility across multiple operating systems and architectures.

---

## ✨ Features

- **IEC 61850 Protocol Support**: Implements key functionalities of the IEC 61850 standard for substation automation, including GOOSE (Generic Object Oriented Substation Events) and MMS (Manufacturing Message Specification).
- **Cross-Platform Compatibility**: Supports Windows, Linux, and macOS with prebuilt binaries for x64, arm, and arm64 architectures.
- **High Performance**: Native C++ implementation optimized for low-latency and reliable data exchange.
- **GOOSE and MMS Support**: Real-time GOOSE message handling and client-server interaction via MMS.
- **File Transfer**: Supports file transfer operations as per IEC 61850 standards.
- **Flexible Integration**: Easy-to-use APIs for integration with Node.js applications, SCADA systems, or custom control solutions.
- **Prebuilt Binaries**: Includes precompiled binaries for Node.js v20, simplifying setup and deployment.
- **Windows DLL Support**: Includes iec61850.dll for Windows, automatically placed alongside addon_iec61850.node for ease of use.

---

## 🖥️ Supported Platforms

| Operating System | Architectures       |
|------------------|--------------------|
| Windows          | x64                |
| Linux            | x64, arm, arm64    |
| macOS            | x64, arm64         |

---

## 🚀 Installation

1. Ensure you have **Node.js v20** installed.
2. Install the package via npm:

   ```bash
   npm install @amigo9090/ih-libiec61850-node --ignore-scripts
   ```

3. Prebuilt binaries will be automatically downloaded for your platform and architecture. If a prebuilt binary is unavailable, the addon will be compiled using `node-gyp`, requiring:
   - **Python 3.11+**
   - A compatible C++ compiler:
     - `gcc` on Linux
     - `MSVC` on Windows
     - `clang` on macOS
4. For Windows: The `iec61850.dll` file is automatically included in the `builds/windows_x64/` directory alongside `addon_iec61850.node`. You need install npcap to the goose support.

---

## 📖 Usage


Below is an example of using `ih-lib61850-node` to establish an IEC 61850 connection and handle GOOSE and MMS messages:

```javascript
const { MmsClient } = require('@amigo9090/ih-libiec61850-node');
const util = require('util');

const client = new MmsClient((event, data) => {
    console.log(`Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    if (event === 'conn' && data.event === 'opened') {
        console.log('Connection opened, browsing data model...');
        client.browseDataModel()
            .then(dataModel => {
                console.log('Data Model:', util.inspect(dataModel, { depth: null }));

                // Extracting links to datasets
                const dataSets = [];
                dataModel.forEach(ld => {
                    ld.logicalNodes.forEach(ln => {
                        ln.dataSets.forEach(ds => {
                            console.log(`Found dataset: ${ds.reference}`);
                            dataSets.push(ds);
                        });
                    });
                });

                // Reading datasets directly without promises
                console.log('Reading datasets...');
                dataSets.forEach(ds => {
                    console.log(`- ${ds.reference}`);
                    client.readDataSetValues(ds.reference);
                });

                // Reading data
                console.log('Reading data...');
                const dataRefs = [
                    'WAGO61850ServerDevice/XCBR1.Pos.stVal',
                    'WAGO61850ServerDevice/GGIO1.Ind.stVal',
                    'WAGO61850ServerDevice/CALH1.GrAlm.stVal'                    
                ];
                dataRefs.forEach(ref => client.readData(ref));               

                // Enabling reporting
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
            ip: '192.168.0.1',
            port: 102,
            clientID: 'mms_client1',
            reconnectDelay: 2
        });
        await sleep(5000);

        // Perform control operation
        console.log('Try to do control operation...');
        client.controlObject("IEC61850ServerDevice/XCBR1.Pos", true);        

        // Waiting for data and reports
        console.log('Waiting for data and reports...');
        await sleep(30000);

        // Disabling reporting
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
```

📚 **Additional Examples**: Examples for all supported functionalities are available in the [`examples/` directory](https://github.com/intrahouseio/ih-lib61850-node/examples). These demonstrate various configurations and use cases for substation automation.

---

## 🛠️ Building from Source

To build the addon from source:

1. Clone the repository:

   ```bash
   git clone https://github.com/intrahouseio/ih-lib61850-node.git
   cd ih-lib61850-node
   ```

2. Install dependencies:

   ```bash
   npm install
   ```

3. Configure and build:

   ```bash
   npm run configure
   npm run build
   ```

4. Optionally, generate prebuilt binaries:

   ```bash
   npm run prebuild
   ```

---

## 🤝 Contributing

Contributions are welcome! To contribute:

1. Fork the repository.
2. Create a new branch for your feature or bug fix.
3. Submit a pull request with a clear description of your changes.

---

## 📜 License

This project is licensed under the [MIT License](https://github.com/intrahouseio/ih-lib61850-node/blob/main/LICENSE).

---

## 💬 Support

For issues, questions, or feature requests, please open an issue on the [GitHub repository](https://github.com/intrahouseio/ih-lib61850-node/issues).