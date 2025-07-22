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
- **Windows DLL Support**: Includes `iec61850.dll` for Windows, automatically placed alongside `addon_iec61850.node` for ease of use.

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
const { IEC61850Client } = require('@amigo9090/ih-libiec61850-node');

// Initialize an IEC 61850 client
const client = new IEC61850Client({
    host: '192.168.0.1',
    port: 102,
    // Additional configuration parameters
});

// Connect to the server
client.connect();

// Subscribe to GOOSE messages
client.subscribeGOOSE('domain', 'gooseId', (message) => {
    console.log('Received GOOSE message:', message);
});

// Send an MMS request
client.sendMMSRequest('domain', 'itemId', (response) => {
    console.log('MMS response:', response);
});

// Handle events
client.on('event', (event) => {
    console.log('Event:', event);
});
```

📚 **Additional Examples**: Examples for all supported functionalities are available in the [`examples/` directory](https://github.com/intrahouseio/ih-lib61850-node/tree/main/examples). These demonstrate various configurations and use cases for substation automation.

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